#include "client.h"
#include "mysql/base/mysql_log.h"
#include "mysql/protoc/builder.h"
#include <concepts>
#include <array>
#include <sys/uio.h>
#include <utility>

namespace galay::mysql
{

namespace detail
{

template<typename Fn>
concept IoErrorCallback = std::invocable<Fn, const IOError&>;

template<typename Fn>
concept VoidCallback = std::invocable<Fn>;

template<typename Fn>
concept ParseErrorCallback = std::invocable<Fn, MysqlError>;

template<typename Fn>
concept ParseFn = requires(Fn&& fn) {
    { std::forward<Fn>(fn)() } -> std::same_as<std::expected<bool, MysqlError>>;
};

inline void syncSendWindow(const std::string& payload, size_t sent, const char*& buffer, size_t& length)
{
    if (sent >= payload.size()) {
        buffer = nullptr;
        length = 0;
        return;
    }
    buffer = payload.data() + sent;
    length = payload.size() - sent;
}

template<typename OnIoError, typename OnZeroSend, typename OnDone>
requires IoErrorCallback<OnIoError> &&
         VoidCallback<OnZeroSend> &&
         VoidCallback<OnDone>
bool handleSendResult(std::expected<size_t, IOError>& io_result,
                      size_t& sent,
                      size_t total,
                      OnIoError&& on_io_error,
                      OnZeroSend&& on_zero_send,
                      OnDone&& on_done)
{
    if (!io_result.has_value()) {
        on_io_error(io_result.error());
        return true;
    }

    const size_t sent_once = io_result.value();
    if (sent_once == 0) {
        on_zero_send();
        return true;
    }

    sent += sent_once;
    if (sent >= total) {
        on_done();
        return true;
    }
    return false;
}

bool prepareRecvWindow(galay::utils::RingBuffer& ring_buffer, std::vector<struct iovec>& iovecs)
{
    struct iovec raw_iovecs[2];
    const size_t count = ring_buffer.getWriteIovecs(raw_iovecs, 2);
    iovecs.assign(raw_iovecs, raw_iovecs + count);
    return !iovecs.empty();
}

template<typename ParseFnType, typename OnParseError>
requires ParseFn<ParseFnType> &&
         ParseErrorCallback<OnParseError>
bool parseOrSetError(ParseFnType&& parse_fn, OnParseError&& on_parse_error)
{
    auto parsed = parse_fn();
    if (!parsed.has_value()) {
        on_parse_error(std::move(parsed.error()));
        return true;
    }
    return parsed.value();
}

template<typename BufferLike, typename OnIoError, typename OnClosed, typename ParseFnType, typename OnParseError>
requires IoErrorCallback<OnIoError> &&
         VoidCallback<OnClosed> &&
         ParseFn<ParseFnType> &&
         ParseErrorCallback<OnParseError>
bool handleReadResult(std::expected<size_t, IOError>& io_result,
                      BufferLike& ring_buffer,
                      OnIoError&& on_io_error,
                      OnClosed&& on_closed,
                      ParseFnType&& parse_fn,
                      OnParseError&& on_parse_error)
{
    if (!io_result.has_value()) {
        on_io_error(io_result.error());
        return true;
    }

    const size_t n = io_result.value();
    if (n == 0) {
        on_closed();
        return true;
    }

    ring_buffer.produce(n);
    return detail::parseOrSetError(std::forward<ParseFnType>(parse_fn),
                                   std::forward<OnParseError>(on_parse_error));
}

MysqlError toTimeoutOrInternalError(const IOError& io_error)
{
    if (IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
        return MysqlError(MYSQL_ERROR_TIMEOUT, io_error.message());
    }
    return MysqlError(MYSQL_ERROR_INTERNAL, io_error.message());
}

MysqlError mapAwaitableIoError(const IOError& io_error, MysqlErrorType fallback_type)
{
    if (IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
        return MysqlError(MYSQL_ERROR_TIMEOUT, io_error.message());
    }
    if (IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
        return MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, io_error.message());
    }
    return MysqlError(fallback_type, io_error.message());
}

inline std::string_view linearizeReadIovecs(std::span<const struct iovec> iovecs, std::string& scratch)
{
    if (iovecs.size() == 1) {
        return std::string_view(static_cast<const char*>(iovecs[0].iov_base),
                                iovecs[0].iov_len);
    }
    scratch.clear();
    for (const auto& iov : iovecs) {
        scratch.append(static_cast<const char*>(iov.iov_base), iov.iov_len);
    }
    return std::string_view(scratch);
}

inline std::string buildSingleCommandPacket(protocol::CommandType cmd,
                                            std::string_view payload,
                                            protocol::MysqlCommandKind kind)
{
    protocol::MysqlCommandBuilder builder;
    builder.reserve(1, protocol::MYSQL_PACKET_HEADER_SIZE + 1 + payload.size());
    builder.appendFast(cmd, payload, 0, kind);
    return std::move(builder.release().encoded);
}

inline std::string encodeRawPacket(std::string_view payload, uint8_t sequence_id)
{
    std::string packet;
    packet.reserve(protocol::MYSQL_PACKET_HEADER_SIZE + payload.size());
    const uint32_t payload_len = static_cast<uint32_t>(payload.size());
    packet.push_back(static_cast<char>(payload_len & 0xFF));
    packet.push_back(static_cast<char>((payload_len >> 8) & 0xFF));
    packet.push_back(static_cast<char>((payload_len >> 16) & 0xFF));
    packet.push_back(static_cast<char>(sequence_id));
    if (!payload.empty()) {
        packet.append(payload.data(), payload.size());
    }
    return packet;
}

#ifdef IOV_MAX
constexpr int kPipelineWritevMaxIov = IOV_MAX > 0 ? IOV_MAX : 1024;
#else
constexpr int kPipelineWritevMaxIov = 1024;
#endif

std::array<struct iovec, 1>& emptyIovecs()
{
    static std::array<struct iovec, 1> empty{};
    return empty;
}

}

// ======================== MysqlConnectAwaitable ========================

MysqlConnectAwaitable::MysqlConnectAwaitable(AsyncMysqlClient& client, MysqlConfig config)
    : m_state(std::make_shared<SharedState>(client, std::move(config)))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

bool MysqlConnectAwaitable::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

MysqlConnectAwaitable::SharedState::SharedState(AsyncMysqlClient& client, MysqlConfig config_in)
    : client(&client)
    , config(std::move(config_in))
    , host(IPType::IPV4, config.host, config.port)
{
    client.ringBuffer().clear();
    auto nonblock_result = client.socket().option().handleNonBlock();
    if (!nonblock_result) {
        result = std::unexpected(MysqlError(
            MYSQL_ERROR_CONNECTION,
            "Failed to set non-blocking before connect: " + nonblock_result.error().message()));
        phase = Phase::Invalid;
    }
}

MysqlConnectAwaitable::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

void MysqlConnectAwaitable::Machine::setError(MysqlError error) noexcept
{
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

void MysqlConnectAwaitable::Machine::setConnectError(const IOError& io_error) noexcept
{
    setError(MysqlError(MYSQL_ERROR_CONNECTION, io_error.message()));
}

void MysqlConnectAwaitable::Machine::setSendError(const IOError& io_error) noexcept
{
    setError(MysqlError(MYSQL_ERROR_SEND, io_error.message()));
}

void MysqlConnectAwaitable::Machine::setRecvError(const std::string& phase, const IOError& io_error) noexcept
{
    setError(MysqlError(MYSQL_ERROR_RECV, io_error.message() + " during " + phase));
}

void MysqlConnectAwaitable::Machine::completeSuccess() noexcept
{
    m_state->connected = true;
    m_state->phase = Phase::Done;
    m_state->result = std::optional<bool>(true);
    MYSQL_LOG_INFO("[client]", "MySQL connected successfully to {}:{}",
                 m_state->config.host,
                 m_state->config.port);
}

bool MysqlConnectAwaitable::Machine::prepareReadWindow()
{
    m_state->read_iov_count = m_state->client->ringBuffer().getWriteIovecs(
        m_state->read_iovecs.data(),
        m_state->read_iovecs.size());
    if (m_state->read_iov_count == 0) {
        const char* phase_name = m_state->phase == Phase::HandshakeRead ? "handshake" : "auth";
        setError(MysqlError(MYSQL_ERROR_RECV,
                            std::string("No writable ring buffer space while waiting ") + phase_name));
        return false;
    }
    return true;
}

galay::kernel::MachineAction<MysqlConnectAwaitable::Result>
MysqlConnectAwaitable::Machine::advance()
{
    if (m_state->result.has_value()) {
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    switch (m_state->phase) {
    case Phase::Invalid:
        setError(MysqlError(MYSQL_ERROR_INTERNAL, "Connect machine entered invalid state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    case Phase::Connect:
        return galay::kernel::MachineAction<result_type>::waitConnect(m_state->host);
    case Phase::HandshakeRead: {
        auto parsed = parseHandshakeFromRingBuffer();
        if (!parsed.has_value()) {
            setError(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepareReadWindow()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::waitReadv(
            m_state->read_iovecs.data(),
            m_state->read_iov_count);
    }
    case Phase::AuthWrite:
        if (m_state->sent >= m_state->auth_packet.size()) {
            m_state->sent = 0;
            m_state->phase = Phase::AuthResultRead;
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        return galay::kernel::MachineAction<result_type>::waitWrite(
            m_state->auth_packet.data() + m_state->sent,
            m_state->auth_packet.size() - m_state->sent);
    case Phase::AuthResultRead: {
        auto parsed = parseAuthResultFromRingBuffer();
        if (!parsed.has_value()) {
            setError(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepareReadWindow()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::waitReadv(
            m_state->read_iovecs.data(),
            m_state->read_iov_count);
    }
    case Phase::Done:
        if (!m_state->result.has_value()) {
            completeSuccess();
        }
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    setError(MysqlError(MYSQL_ERROR_INTERNAL, "Unknown connect machine state"));
    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
}

void MysqlConnectAwaitable::Machine::onConnect(std::expected<void, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        setConnectError(result.error());
        return;
    }

    auto nonblock_result = m_state->client->socket().option().handleNonBlock();
    if (!nonblock_result) {
        setError(MysqlError(MYSQL_ERROR_CONNECTION,
                            "Failed to keep non-blocking after connect: " +
                                nonblock_result.error().message()));
        return;
    }

    m_state->phase = Phase::HandshakeRead;
}

void MysqlConnectAwaitable::Machine::onRead(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }

    const char* phase_name = m_state->phase == Phase::HandshakeRead ? "handshake" : "auth";
    if (!result.has_value()) {
        setRecvError(phase_name, result.error());
        return;
    }

    const size_t n = result.value();
    if (n == 0) {
        setError(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED,
                            std::string("Connection closed during ") + phase_name));
        return;
    }

    m_state->client->ringBuffer().produce(n);

    auto parsed = m_state->phase == Phase::HandshakeRead
        ? parseHandshakeFromRingBuffer()
        : parseAuthResultFromRingBuffer();
    if (!parsed.has_value()) {
        setError(std::move(parsed.error()));
    }
}

void MysqlConnectAwaitable::Machine::onWrite(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        setSendError(result.error());
        return;
    }

    const size_t sent_once = result.value();
    if (sent_once == 0) {
        setError(MysqlError(MYSQL_ERROR_SEND, "Send returned 0 bytes"));
        return;
    }

    m_state->sent += sent_once;
    if (m_state->sent >= m_state->auth_packet.size()) {
        m_state->sent = 0;
        m_state->phase = Phase::AuthResultRead;
    }
}

std::expected<bool, MysqlError> MysqlConnectAwaitable::Machine::parseHandshakeFromRingBuffer()
{
    struct iovec read_iovecs[2];
    const size_t read_iovecs_count = m_state->client->ringBuffer().getReadIovecs(read_iovecs, 2);
    if (read_iovecs_count == 0) {
        return false;
    }

    auto linear = detail::linearizeReadIovecs(
        std::span<const struct iovec>(read_iovecs, read_iovecs_count),
        m_state->parse_scratch);
    const char* data = linear.data();
    size_t len = linear.size();

    size_t consumed = 0;
    auto pkt = m_state->client->parser().extractPacket(data, len, consumed);
    if (!pkt) {
        if (pkt.error() == protocol::ParseError::Incomplete) {
            return false;
        }
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse handshake packet"));
    }

    if (static_cast<uint8_t>(pkt->payload[0]) == 0xFF) {
        auto err = m_state->client->parser().parseErr(
            pkt->payload,
            pkt->payload_len,
            protocol::CLIENT_PROTOCOL_41);
        m_state->client->ringBuffer().consume(consumed);
        if (err) {
            return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
        }
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse handshake ERR packet"));
    }

    auto hs = m_state->client->parser().parseHandshake(pkt->payload, pkt->payload_len);
    m_state->client->ringBuffer().consume(consumed);
    if (!hs) {
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse handshake packet body"));
    }
    m_state->handshake = std::move(hs.value());

    protocol::HandshakeResponse41 resp;
    resp.capability_flags = protocol::CLIENT_PROTOCOL_41
        | protocol::CLIENT_SECURE_CONNECTION
        | protocol::CLIENT_PLUGIN_AUTH
        | protocol::CLIENT_TRANSACTIONS
        | protocol::CLIENT_MULTI_STATEMENTS
        | protocol::CLIENT_MULTI_RESULTS
        | protocol::CLIENT_PS_MULTI_RESULTS
        | protocol::CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA;
    if (!m_state->config.database.empty()) {
        resp.capability_flags |= protocol::CLIENT_CONNECT_WITH_DB;
    }
    resp.capability_flags &= m_state->handshake.capability_flags;
    m_state->client->setServerCapabilities(resp.capability_flags);
    resp.character_set = protocol::CHARSET_UTF8MB4_GENERAL_CI;
    resp.username = m_state->config.username;
    resp.database = m_state->config.database;
    resp.auth_plugin_name = m_state->handshake.auth_plugin_name;

    if (m_state->handshake.auth_plugin_name == "mysql_native_password") {
        resp.auth_response = protocol::AuthPlugin::nativePasswordAuth(
            m_state->config.password,
            m_state->handshake.auth_plugin_data);
    } else if (m_state->handshake.auth_plugin_name == "caching_sha2_password") {
        resp.auth_response = protocol::AuthPlugin::cachingSha2Auth(
            m_state->config.password,
            m_state->handshake.auth_plugin_data);
    } else {
        resp.auth_response = protocol::AuthPlugin::nativePasswordAuth(
            m_state->config.password,
            m_state->handshake.auth_plugin_data);
        resp.auth_plugin_name = "mysql_native_password";
    }

    m_state->auth_stage = AuthStage::InitialResponse;
    m_state->auth_packet =
        m_state->client->encoder().encodeHandshakeResponse(resp, pkt->sequence_id + 1);
    m_state->sent = 0;
    m_state->phase = Phase::AuthWrite;
    return true;
}

std::expected<bool, MysqlError> MysqlConnectAwaitable::Machine::parseAuthResultFromRingBuffer()
{
    while (true) {
        struct iovec read_iovecs[2];
        const size_t read_iovecs_count = m_state->client->ringBuffer().getReadIovecs(read_iovecs, 2);
        if (read_iovecs_count == 0) {
            return false;
        }

        auto linear = detail::linearizeReadIovecs(
            std::span<const struct iovec>(read_iovecs, read_iovecs_count),
            m_state->parse_scratch);
        const char* data = linear.data();
        size_t len = linear.size();

        size_t consumed = 0;
        auto pkt = m_state->client->parser().extractPacket(data, len, consumed);
        if (!pkt) {
            if (pkt.error() == protocol::ParseError::Incomplete) {
                return false;
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse auth response packet"));
        }

        const uint8_t first_byte = static_cast<uint8_t>(pkt->payload[0]);
        m_state->client->ringBuffer().consume(consumed);

        if (m_state->auth_stage == AuthStage::AwaitPublicKey) {
            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parseErr(
                    pkt->payload,
                    pkt->payload_len,
                    m_state->client->serverCapabilities());
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Public key request failed"));
            }

            std::string_view public_key(pkt->payload, pkt->payload_len);
            if (!public_key.empty() && static_cast<uint8_t>(public_key.front()) == 0x01) {
                public_key.remove_prefix(1);
            }

            auto encrypted = protocol::AuthPlugin::cachingSha2FullAuth(
                m_state->config.password,
                m_state->handshake.auth_plugin_data,
                public_key);
            if (!encrypted) {
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, encrypted.error()));
            }

            m_state->auth_packet = detail::encodeRawPacket(*encrypted, pkt->sequence_id + 1);
            m_state->sent = 0;
            m_state->auth_stage = AuthStage::AwaitFinalResult;
            m_state->phase = Phase::AuthWrite;
            return true;
        }

        if (first_byte == 0x00) {
            completeSuccess();
            return true;
        }

        if (first_byte == 0xFF) {
            auto err = m_state->client->parser().parseErr(
                pkt->payload,
                pkt->payload_len,
                m_state->client->serverCapabilities());
            if (err) {
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, err->error_code, err->error_message));
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Authentication failed"));
        }

        if (first_byte == 0x01) {
            if (pkt->payload_len == 2 && static_cast<uint8_t>(pkt->payload[1]) == 0x03) {
                m_state->auth_stage = AuthStage::AwaitFastAuthResult;
                continue;
            }
            if (pkt->payload_len == 2 &&
                static_cast<uint8_t>(pkt->payload[1]) == 0x04 &&
                m_state->handshake.auth_plugin_name == "caching_sha2_password") {
                static const std::string kPublicKeyRequest(1, '\x02');
                m_state->auth_packet = detail::encodeRawPacket(kPublicKeyRequest, pkt->sequence_id + 1);
                m_state->sent = 0;
                m_state->auth_stage = AuthStage::AwaitPublicKey;
                m_state->phase = Phase::AuthWrite;
                return true;
            }
            return std::unexpected(MysqlError(
                MYSQL_ERROR_AUTH,
                "Full authentication not supported, use mysql_native_password"));
        }

        if (first_byte == 0xFE) {
            return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Auth switch is not supported"));
        }

        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Unexpected auth response packet"));
    }
}

// ======================== MysqlQueryAwaitable ========================

MysqlQueryAwaitable::MysqlQueryAwaitable(AsyncMysqlClient& client, std::string_view sql)
    : m_state(std::make_shared<SharedState>(client, sql))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

bool MysqlQueryAwaitable::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

MysqlQueryAwaitable::SharedState::SharedState(AsyncMysqlClient& client, std::string_view sql)
    : client(&client)
    , encoded_cmd(detail::buildSingleCommandPacket(protocol::CommandType::COM_QUERY,
                                                   sql,
                                                   protocol::MysqlCommandKind::Query))
{
    if (client.asyncConfig().result_row_reserve_hint > 0) {
        result_set.reserveRows(client.asyncConfig().result_row_reserve_hint);
    }
}

MysqlQueryAwaitable::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

void MysqlQueryAwaitable::Machine::setError(MysqlError error) noexcept
{
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

void MysqlQueryAwaitable::Machine::setSendError(const IOError& io_error) noexcept
{
    MYSQL_LOG_DEBUG("[client]", "send query failed: {}", io_error.message());
    setError(detail::mapAwaitableIoError(io_error, MYSQL_ERROR_SEND));
}

void MysqlQueryAwaitable::Machine::setRecvError(const IOError& io_error) noexcept
{
    MYSQL_LOG_DEBUG("[client]", "recv query failed: {}", io_error.message());
    setError(detail::mapAwaitableIoError(io_error, MYSQL_ERROR_RECV));
}

bool MysqlQueryAwaitable::Machine::prepareReadWindow()
{
    m_state->read_iov_count = m_state->client->ringBuffer().getWriteIovecs(
        m_state->read_iovecs.data(),
        m_state->read_iovecs.size());
    if (m_state->read_iov_count == 0) {
        setError(MysqlError(MYSQL_ERROR_RECV, "No writable ring buffer space"));
        return false;
    }
    return true;
}

galay::kernel::MachineAction<MysqlQueryAwaitable::Result>
MysqlQueryAwaitable::Machine::advance()
{
    if (m_state->result.has_value()) {
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    switch (m_state->phase) {
    case Phase::Invalid:
        setError(MysqlError(MYSQL_ERROR_INTERNAL, "Query machine entered invalid state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    case Phase::SendCommand:
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->client->ringBuffer().clear();
            m_state->phase = Phase::ReceivingHeader;
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        return galay::kernel::MachineAction<result_type>::waitWrite(
            m_state->encoded_cmd.data() + m_state->sent,
            m_state->encoded_cmd.size() - m_state->sent);
    case Phase::ReceivingHeader:
    case Phase::ReceivingColumns:
    case Phase::ReceivingColumnEof:
    case Phase::ReceivingRows: {
        auto parsed = tryParseFromRingBuffer();
        if (!parsed.has_value()) {
            setError(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepareReadWindow()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::waitReadv(
            m_state->read_iovecs.data(),
            m_state->read_iov_count);
    }
    case Phase::Done:
        if (!m_state->result.has_value()) {
            m_state->result = std::optional<MysqlResultSet>(std::move(m_state->result_set));
        }
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    setError(MysqlError(MYSQL_ERROR_INTERNAL, "Unknown query machine state"));
    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
}

void MysqlQueryAwaitable::Machine::onRead(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        setRecvError(result.error());
        return;
    }
    if (result.value() == 0) {
        setError(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, "Connection closed"));
        return;
    }
    m_state->client->ringBuffer().produce(result.value());
}

void MysqlQueryAwaitable::Machine::onWrite(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        setSendError(result.error());
        return;
    }
    if (result.value() == 0) {
        setError(MysqlError(MYSQL_ERROR_SEND, "Send returned 0 bytes"));
        return;
    }
    m_state->sent += result.value();
    if (m_state->sent >= m_state->encoded_cmd.size()) {
        m_state->client->ringBuffer().clear();
        m_state->phase = Phase::ReceivingHeader;
    }
}

std::expected<bool, MysqlError> MysqlQueryAwaitable::Machine::tryParseFromRingBuffer()
{
    while (true) {
        struct iovec read_iovecs[2];
        const size_t read_iovecs_count = m_state->client->ringBuffer().getReadIovecs(read_iovecs, 2);
        if (read_iovecs_count == 0) {
            return false;
        }

        auto linear = detail::linearizeReadIovecs(
            std::span<const struct iovec>(read_iovecs, read_iovecs_count),
            m_state->parse_scratch);
        const char* data = linear.data();
        size_t len = linear.size();

        size_t consumed = 0;
        auto pkt = m_state->client->parser().extractPacket(data, len, consumed);
        if (!pkt) {
            if (pkt.error() == protocol::ParseError::Incomplete) {
                return false;
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse query packet failed"));
        }

        const uint8_t first_byte = static_cast<uint8_t>(pkt->payload[0]);
        const uint32_t caps = m_state->client->serverCapabilities();

        if (m_state->phase == Phase::ReceivingHeader) {
            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parseErr(pkt->payload, pkt->payload_len, caps);
                m_state->client->ringBuffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Query failed"));
            }

            if (first_byte == 0x00) {
                auto ok = m_state->client->parser().parseOk(pkt->payload, pkt->payload_len, caps);
                m_state->client->ringBuffer().consume(consumed);
                if (!ok) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse OK packet"));
                }
                m_state->result_set.setAffectedRows(ok->affected_rows);
                m_state->result_set.setLastInsertId(ok->last_insert_id);
                m_state->result_set.setWarnings(ok->warnings);
                m_state->result_set.setStatusFlags(ok->status_flags);
                m_state->result_set.setInfo(ok->info);
                m_state->phase = Phase::Done;
                return true;
            }

            size_t int_consumed = 0;
            auto col_count = protocol::readLenEncInt(pkt->payload, pkt->payload_len, int_consumed);
            if (!col_count) {
                m_state->client->ringBuffer().consume(consumed);
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse column count"));
            }

            m_state->column_count = col_count.value();
            m_state->columns_received = 0;
            m_state->result_set.reserveFields(static_cast<size_t>(m_state->column_count));
            m_state->client->ringBuffer().consume(consumed);
            m_state->phase = Phase::ReceivingColumns;
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumns) {
            auto col = m_state->client->parser().parseColumnDefinition(pkt->payload, pkt->payload_len);
            m_state->client->ringBuffer().consume(consumed);
            if (!col) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse column definition"));
            }

            MysqlField field(col->name,
                             static_cast<MysqlFieldType>(col->column_type),
                             col->flags,
                             col->column_length,
                             col->decimals);
            field.setCatalog(col->catalog);
            field.setSchema(col->schema);
            field.setTable(col->table);
            field.setOrgTable(col->org_table);
            field.setOrgName(col->org_name);
            field.setCharacterSet(col->character_set);
            m_state->result_set.addField(std::move(field));

            ++m_state->columns_received;
            if (m_state->columns_received >= m_state->column_count) {
                m_state->phase = (caps & protocol::CLIENT_DEPRECATE_EOF)
                    ? Phase::ReceivingRows
                    : Phase::ReceivingColumnEof;
            }
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumnEof) {
            m_state->client->ringBuffer().consume(consumed);
            m_state->phase = Phase::ReceivingRows;
            continue;
        }

        if (m_state->phase == Phase::ReceivingRows) {
            if (first_byte == 0xFE && pkt->payload_len < 0xFFFFFF) {
                if (caps & protocol::CLIENT_DEPRECATE_EOF) {
                    auto ok = m_state->client->parser().parseOk(pkt->payload, pkt->payload_len, caps);
                    if (ok) {
                        m_state->result_set.setWarnings(ok->warnings);
                        m_state->result_set.setStatusFlags(ok->status_flags);
                    }
                } else {
                    auto eof = m_state->client->parser().parseEof(pkt->payload, pkt->payload_len);
                    if (eof) {
                        m_state->result_set.setWarnings(eof->warnings);
                        m_state->result_set.setStatusFlags(eof->status_flags);
                    }
                }
                m_state->client->ringBuffer().consume(consumed);
                m_state->phase = Phase::Done;
                return true;
            }

            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parseErr(pkt->payload, pkt->payload_len, caps);
                m_state->client->ringBuffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Error during row fetch"));
            }

            auto row = m_state->client->parser().parseTextRow(
                pkt->payload,
                pkt->payload_len,
                m_state->column_count);
            m_state->client->ringBuffer().consume(consumed);
            if (!row) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse text row"));
            }
            m_state->result_set.addRow(MysqlRow(std::move(row.value())));
            continue;
        }

        return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Invalid query parser state"));
    }
}

// ======================== MysqlPrepareAwaitable ========================

MysqlPrepareAwaitable::MysqlPrepareAwaitable(AsyncMysqlClient& client, std::string_view sql)
    : m_state(std::make_shared<SharedState>(client, sql))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

bool MysqlPrepareAwaitable::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

MysqlPrepareAwaitable::SharedState::SharedState(AsyncMysqlClient& client, std::string_view sql)
    : client(&client)
    , encoded_cmd(detail::buildSingleCommandPacket(protocol::CommandType::COM_STMT_PREPARE,
                                                   sql,
                                                   protocol::MysqlCommandKind::StmtPrepare))
{
}

MysqlPrepareAwaitable::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

void MysqlPrepareAwaitable::Machine::setError(MysqlError error) noexcept
{
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

void MysqlPrepareAwaitable::Machine::setSendError(const IOError& io_error) noexcept
{
    setError(detail::mapAwaitableIoError(io_error, MYSQL_ERROR_SEND));
}

void MysqlPrepareAwaitable::Machine::setRecvError(const IOError& io_error) noexcept
{
    setError(detail::mapAwaitableIoError(io_error, MYSQL_ERROR_RECV));
}

bool MysqlPrepareAwaitable::Machine::prepareReadWindow()
{
    m_state->read_iov_count = m_state->client->ringBuffer().getWriteIovecs(
        m_state->read_iovecs.data(),
        m_state->read_iovecs.size());
    if (m_state->read_iov_count == 0) {
        setError(MysqlError(MYSQL_ERROR_RECV, "No writable ring buffer space"));
        return false;
    }
    return true;
}

galay::kernel::MachineAction<MysqlPrepareAwaitable::Result>
MysqlPrepareAwaitable::Machine::advance()
{
    if (m_state->result.has_value()) {
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    switch (m_state->phase) {
    case Phase::Invalid:
        setError(MysqlError(MYSQL_ERROR_INTERNAL, "Prepare machine entered invalid state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    case Phase::SendCommand:
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->client->ringBuffer().clear();
            m_state->phase = Phase::ReceivingPrepareOk;
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        return galay::kernel::MachineAction<result_type>::waitWrite(
            m_state->encoded_cmd.data() + m_state->sent,
            m_state->encoded_cmd.size() - m_state->sent);
    case Phase::ReceivingPrepareOk:
    case Phase::ReceivingParamDefs:
    case Phase::ReceivingParamEof:
    case Phase::ReceivingColumnDefs:
    case Phase::ReceivingColumnEof: {
        auto parsed = tryParseFromRingBuffer();
        if (!parsed.has_value()) {
            setError(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepareReadWindow()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::waitReadv(
            m_state->read_iovecs.data(),
            m_state->read_iov_count);
    }
    case Phase::Done:
        if (!m_state->result.has_value()) {
            m_state->result = std::optional<PrepareResult>(std::move(m_state->prepare_result));
        }
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    setError(MysqlError(MYSQL_ERROR_INTERNAL, "Unknown prepare machine state"));
    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
}

void MysqlPrepareAwaitable::Machine::onRead(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        setRecvError(result.error());
        return;
    }
    if (result.value() == 0) {
        setError(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, "Connection closed"));
        return;
    }
    m_state->client->ringBuffer().produce(result.value());
}

void MysqlPrepareAwaitable::Machine::onWrite(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        setSendError(result.error());
        return;
    }
    if (result.value() == 0) {
        setError(MysqlError(MYSQL_ERROR_SEND, "Send returned 0 bytes"));
        return;
    }
    m_state->sent += result.value();
    if (m_state->sent >= m_state->encoded_cmd.size()) {
        m_state->client->ringBuffer().clear();
        m_state->phase = Phase::ReceivingPrepareOk;
    }
}

std::expected<bool, MysqlError> MysqlPrepareAwaitable::Machine::tryParseFromRingBuffer()
{
    while (true) {
        struct iovec read_iovecs[2];
        const size_t read_iovecs_count = m_state->client->ringBuffer().getReadIovecs(read_iovecs, 2);
        if (read_iovecs_count == 0) {
            return false;
        }

        auto linear = detail::linearizeReadIovecs(
            std::span<const struct iovec>(read_iovecs, read_iovecs_count),
            m_state->parse_scratch);
        const char* data = linear.data();
        size_t len = linear.size();

        size_t consumed = 0;
        auto pkt = m_state->client->parser().extractPacket(data, len, consumed);
        if (!pkt) {
            if (pkt.error() == protocol::ParseError::Incomplete) {
                return false;
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse prepare packet failed"));
        }

        const uint8_t first_byte = static_cast<uint8_t>(pkt->payload[0]);
        const uint32_t caps = m_state->client->serverCapabilities();

        if (m_state->phase == Phase::ReceivingPrepareOk) {
            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parseErr(pkt->payload, pkt->payload_len, caps);
                m_state->client->ringBuffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_PREPARED_STMT, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_PREPARED_STMT, "Prepare failed"));
            }

            auto ok = m_state->client->parser().parseStmtPrepareOk(pkt->payload, pkt->payload_len);
            m_state->client->ringBuffer().consume(consumed);
            if (!ok) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse COM_STMT_PREPARE OK"));
            }

            m_state->prepare_result.statement_id = ok->statement_id;
            m_state->prepare_result.num_params = ok->num_params;
            m_state->prepare_result.num_columns = ok->num_columns;
            m_state->prepare_result.param_fields.reserve(m_state->prepare_result.num_params);
            m_state->prepare_result.column_fields.reserve(m_state->prepare_result.num_columns);
            m_state->params_received = 0;
            m_state->columns_received = 0;

            if (ok->num_params > 0) {
                m_state->phase = Phase::ReceivingParamDefs;
                continue;
            }
            if (ok->num_columns > 0) {
                m_state->phase = Phase::ReceivingColumnDefs;
                continue;
            }
            m_state->phase = Phase::Done;
            return true;
        }

        if (m_state->phase == Phase::ReceivingParamDefs) {
            auto col = m_state->client->parser().parseColumnDefinition(pkt->payload, pkt->payload_len);
            m_state->client->ringBuffer().consume(consumed);
            if (!col) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse parameter definition failed"));
            }

            MysqlField field(col->name,
                             static_cast<MysqlFieldType>(col->column_type),
                             col->flags,
                             col->column_length,
                             col->decimals);
            m_state->prepare_result.param_fields.push_back(std::move(field));
            ++m_state->params_received;
            if (m_state->params_received >= m_state->prepare_result.num_params) {
                m_state->phase = Phase::ReceivingParamEof;
            }
            continue;
        }

        if (m_state->phase == Phase::ReceivingParamEof) {
            m_state->client->ringBuffer().consume(consumed);
            if (m_state->prepare_result.num_columns > 0) {
                m_state->phase = Phase::ReceivingColumnDefs;
                continue;
            }
            m_state->phase = Phase::Done;
            return true;
        }

        if (m_state->phase == Phase::ReceivingColumnDefs) {
            auto col = m_state->client->parser().parseColumnDefinition(pkt->payload, pkt->payload_len);
            m_state->client->ringBuffer().consume(consumed);
            if (!col) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse column definition failed"));
            }

            MysqlField field(col->name,
                             static_cast<MysqlFieldType>(col->column_type),
                             col->flags,
                             col->column_length,
                             col->decimals);
            m_state->prepare_result.column_fields.push_back(std::move(field));
            ++m_state->columns_received;
            if (m_state->columns_received >= m_state->prepare_result.num_columns) {
                m_state->phase = Phase::ReceivingColumnEof;
            }
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumnEof) {
            m_state->client->ringBuffer().consume(consumed);
            m_state->phase = Phase::Done;
            return true;
        }

        return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Invalid prepare parser state"));
    }
}

// ======================== MysqlStmtExecuteAwaitable ========================

MysqlStmtExecuteAwaitable::MysqlStmtExecuteAwaitable(AsyncMysqlClient& client, std::string encoded_cmd)
    : m_state(std::make_shared<SharedState>(client, std::move(encoded_cmd)))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

bool MysqlStmtExecuteAwaitable::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

MysqlStmtExecuteAwaitable::SharedState::SharedState(AsyncMysqlClient& client, std::string encoded_cmd_in)
    : client(&client)
    , encoded_cmd(std::move(encoded_cmd_in))
{
    if (client.asyncConfig().result_row_reserve_hint > 0) {
        result_set.reserveRows(client.asyncConfig().result_row_reserve_hint);
    }
}

MysqlStmtExecuteAwaitable::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

void MysqlStmtExecuteAwaitable::Machine::setError(MysqlError error) noexcept
{
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

void MysqlStmtExecuteAwaitable::Machine::setSendError(const IOError& io_error) noexcept
{
    setError(detail::mapAwaitableIoError(io_error, MYSQL_ERROR_SEND));
}

void MysqlStmtExecuteAwaitable::Machine::setRecvError(const IOError& io_error) noexcept
{
    setError(detail::mapAwaitableIoError(io_error, MYSQL_ERROR_RECV));
}

bool MysqlStmtExecuteAwaitable::Machine::prepareReadWindow()
{
    m_state->read_iov_count = m_state->client->ringBuffer().getWriteIovecs(
        m_state->read_iovecs.data(),
        m_state->read_iovecs.size());
    if (m_state->read_iov_count == 0) {
        setError(MysqlError(MYSQL_ERROR_RECV, "No writable ring buffer space"));
        return false;
    }
    return true;
}

galay::kernel::MachineAction<MysqlStmtExecuteAwaitable::Result>
MysqlStmtExecuteAwaitable::Machine::advance()
{
    if (m_state->result.has_value()) {
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    switch (m_state->phase) {
    case Phase::Invalid:
        setError(MysqlError(MYSQL_ERROR_INTERNAL, "StmtExecute machine entered invalid state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    case Phase::SendCommand:
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->client->ringBuffer().clear();
            m_state->phase = Phase::ReceivingHeader;
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        return galay::kernel::MachineAction<result_type>::waitWrite(
            m_state->encoded_cmd.data() + m_state->sent,
            m_state->encoded_cmd.size() - m_state->sent);
    case Phase::ReceivingHeader:
    case Phase::ReceivingColumns:
    case Phase::ReceivingColumnEof:
    case Phase::ReceivingRows: {
        auto parsed = tryParseFromRingBuffer();
        if (!parsed.has_value()) {
            setError(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepareReadWindow()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::waitReadv(
            m_state->read_iovecs.data(),
            m_state->read_iov_count);
    }
    case Phase::Done:
        if (!m_state->result.has_value()) {
            m_state->result = std::optional<MysqlResultSet>(std::move(m_state->result_set));
        }
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    setError(MysqlError(MYSQL_ERROR_INTERNAL, "Unknown stmt-execute machine state"));
    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
}

void MysqlStmtExecuteAwaitable::Machine::onRead(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        setRecvError(result.error());
        return;
    }
    if (result.value() == 0) {
        setError(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, "Connection closed"));
        return;
    }
    m_state->client->ringBuffer().produce(result.value());
}

void MysqlStmtExecuteAwaitable::Machine::onWrite(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        setSendError(result.error());
        return;
    }
    if (result.value() == 0) {
        setError(MysqlError(MYSQL_ERROR_SEND, "Send returned 0 bytes"));
        return;
    }
    m_state->sent += result.value();
    if (m_state->sent >= m_state->encoded_cmd.size()) {
        m_state->client->ringBuffer().clear();
        m_state->phase = Phase::ReceivingHeader;
    }
}

std::expected<bool, MysqlError> MysqlStmtExecuteAwaitable::Machine::tryParseFromRingBuffer()
{
    while (true) {
        struct iovec read_iovecs[2];
        const size_t read_iovecs_count = m_state->client->ringBuffer().getReadIovecs(read_iovecs, 2);
        if (read_iovecs_count == 0) {
            return false;
        }

        auto linear = detail::linearizeReadIovecs(
            std::span<const struct iovec>(read_iovecs, read_iovecs_count),
            m_state->parse_scratch);
        const char* data = linear.data();
        size_t len = linear.size();

        size_t consumed = 0;
        auto pkt = m_state->client->parser().extractPacket(data, len, consumed);
        if (!pkt) {
            if (pkt.error() == protocol::ParseError::Incomplete) {
                return false;
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse stmt-execute packet failed"));
        }

        const uint8_t first_byte = static_cast<uint8_t>(pkt->payload[0]);
        const uint32_t caps = m_state->client->serverCapabilities();

        if (m_state->phase == Phase::ReceivingHeader) {
            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parseErr(pkt->payload, pkt->payload_len, caps);
                m_state->client->ringBuffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Execute failed"));
            }

            if (first_byte == 0x00) {
                auto ok = m_state->client->parser().parseOk(pkt->payload, pkt->payload_len, caps);
                m_state->client->ringBuffer().consume(consumed);
                if (!ok) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse OK packet"));
                }
                m_state->result_set.setAffectedRows(ok->affected_rows);
                m_state->result_set.setLastInsertId(ok->last_insert_id);
                m_state->result_set.setWarnings(ok->warnings);
                m_state->result_set.setStatusFlags(ok->status_flags);
                m_state->phase = Phase::Done;
                return true;
            }

            size_t int_consumed = 0;
            auto col_count = protocol::readLenEncInt(pkt->payload, pkt->payload_len, int_consumed);
            if (!col_count) {
                m_state->client->ringBuffer().consume(consumed);
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse column count"));
            }
            m_state->column_count = col_count.value();
            m_state->columns_received = 0;
            m_state->result_set.reserveFields(static_cast<size_t>(m_state->column_count));
            m_state->client->ringBuffer().consume(consumed);
            m_state->phase = Phase::ReceivingColumns;
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumns) {
            auto col = m_state->client->parser().parseColumnDefinition(pkt->payload, pkt->payload_len);
            m_state->client->ringBuffer().consume(consumed);
            if (!col) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse column definition failed"));
            }

            MysqlField field(col->name,
                             static_cast<MysqlFieldType>(col->column_type),
                             col->flags,
                             col->column_length,
                             col->decimals);
            field.setCatalog(col->catalog);
            field.setSchema(col->schema);
            field.setTable(col->table);
            field.setOrgTable(col->org_table);
            field.setOrgName(col->org_name);
            field.setCharacterSet(col->character_set);
            m_state->result_set.addField(std::move(field));

            ++m_state->columns_received;
            if (m_state->columns_received >= m_state->column_count) {
                m_state->phase = (caps & protocol::CLIENT_DEPRECATE_EOF)
                    ? Phase::ReceivingRows
                    : Phase::ReceivingColumnEof;
            }
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumnEof) {
            m_state->client->ringBuffer().consume(consumed);
            m_state->phase = Phase::ReceivingRows;
            continue;
        }

        if (m_state->phase == Phase::ReceivingRows) {
            if (first_byte == 0xFE && pkt->payload_len < 0xFFFFFF) {
                if (caps & protocol::CLIENT_DEPRECATE_EOF) {
                    auto ok = m_state->client->parser().parseOk(pkt->payload, pkt->payload_len, caps);
                    if (ok) {
                        m_state->result_set.setWarnings(ok->warnings);
                        m_state->result_set.setStatusFlags(ok->status_flags);
                    }
                } else {
                    auto eof = m_state->client->parser().parseEof(pkt->payload, pkt->payload_len);
                    if (eof) {
                        m_state->result_set.setWarnings(eof->warnings);
                        m_state->result_set.setStatusFlags(eof->status_flags);
                    }
                }
                m_state->client->ringBuffer().consume(consumed);
                m_state->phase = Phase::Done;
                return true;
            }

            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parseErr(pkt->payload, pkt->payload_len, caps);
                m_state->client->ringBuffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Error during row fetch"));
            }

            auto row = m_state->client->parser().parseTextRow(
                pkt->payload,
                pkt->payload_len,
                m_state->column_count);
            m_state->client->ringBuffer().consume(consumed);
            if (!row) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse row failed"));
            }
            m_state->result_set.addRow(MysqlRow(std::move(row.value())));
            continue;
        }

        return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Invalid stmt-execute parser state"));
    }
}

// ======================== MysqlPipelineAwaitable ========================

MysqlPipelineAwaitable::MysqlPipelineAwaitable(AsyncMysqlClient& client,
                                               std::span<const protocol::MysqlCommandView> commands)
    : m_state(std::make_shared<SharedState>(client, commands))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

bool MysqlPipelineAwaitable::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

MysqlPipelineAwaitable::SharedState::SharedState(
    AsyncMysqlClient& client,
    std::span<const protocol::MysqlCommandView> commands)
    : client(&client)
    , expected_results(commands.size())
    , phase(commands.empty() ? Phase::Done : Phase::SendCommands)
{
    results.reserve(expected_results);
    if (client.asyncConfig().result_row_reserve_hint > 0) {
        current_result.reserveRows(client.asyncConfig().result_row_reserve_hint);
    }

    if (commands.empty()) {
        result = std::optional<std::vector<MysqlResultSet>>(std::vector<MysqlResultSet>{});
        return;
    }

    size_t encoded_bytes = 0;
    for (const auto& cmd : commands) {
        if (cmd.encoded.empty()) {
            result = std::unexpected(
                MysqlError(MYSQL_ERROR_PROTOCOL, "Pipeline command encoded payload is empty"));
            phase = Phase::Invalid;
            return;
        }
        encoded_bytes += cmd.encoded.size();
    }

    encoded_buffer.reserve(encoded_bytes);
    encoded_slices.reserve(commands.size());
    for (const auto& cmd : commands) {
        const size_t offset = encoded_buffer.size();
        encoded_buffer.append(cmd.encoded.data(), cmd.encoded.size());
        encoded_slices.push_back(EncodedSlice{offset, cmd.encoded.size()});
    }

    const size_t reserve_hint = encoded_slices.size() <
                                        static_cast<size_t>(detail::kPipelineWritevMaxIov)
                                    ? encoded_slices.size()
                                    : static_cast<size_t>(detail::kPipelineWritevMaxIov);
    write_iovecs.reserve(reserve_hint);
}

MysqlPipelineAwaitable::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

void MysqlPipelineAwaitable::Machine::setError(MysqlError error) noexcept
{
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

void MysqlPipelineAwaitable::Machine::setSendError(const IOError& io_error) noexcept
{
    setError(detail::mapAwaitableIoError(io_error, MYSQL_ERROR_SEND));
}

void MysqlPipelineAwaitable::Machine::setRecvError(const IOError& io_error) noexcept
{
    setError(detail::mapAwaitableIoError(io_error, MYSQL_ERROR_RECV));
}

void MysqlPipelineAwaitable::Machine::refillWriteIovWindow()
{
    if (m_state->write_iov_cursor > 0) {
        m_state->write_iovecs.erase(
            m_state->write_iovecs.begin(),
            m_state->write_iovecs.begin() +
                static_cast<std::vector<struct iovec>::difference_type>(m_state->write_iov_cursor));
        m_state->write_iov_cursor = 0;
    }

    while (m_state->write_iovecs.size() < static_cast<size_t>(detail::kPipelineWritevMaxIov) &&
           m_state->next_command_index < m_state->encoded_slices.size()) {
        const auto encoded_slice = m_state->encoded_slices[m_state->next_command_index++];
        if (encoded_slice.length == 0) {
            continue;
        }

        struct iovec iov{};
        iov.iov_base = const_cast<char*>(m_state->encoded_buffer.data() + encoded_slice.offset);
        iov.iov_len = encoded_slice.length;
        m_state->write_iovecs.push_back(iov);
    }
}

size_t MysqlPipelineAwaitable::Machine::pendingWriteIovCount()
{
    while (m_state->write_iov_cursor < m_state->write_iovecs.size() &&
           m_state->write_iovecs[m_state->write_iov_cursor].iov_len == 0) {
        ++m_state->write_iov_cursor;
    }

    if (m_state->write_iov_cursor >= m_state->write_iovecs.size()) {
        refillWriteIovWindow();
        while (m_state->write_iov_cursor < m_state->write_iovecs.size() &&
               m_state->write_iovecs[m_state->write_iov_cursor].iov_len == 0) {
            ++m_state->write_iov_cursor;
        }
    }

    if (m_state->write_iov_cursor >= m_state->write_iovecs.size()) {
        return 0;
    }

    return m_state->write_iovecs.size() - m_state->write_iov_cursor;
}

bool MysqlPipelineAwaitable::Machine::advanceAfterWrite(size_t sent_bytes)
{
    size_t remaining = sent_bytes;
    while (remaining > 0 && m_state->write_iov_cursor < m_state->write_iovecs.size()) {
        auto& iov = m_state->write_iovecs[m_state->write_iov_cursor];
        if (iov.iov_len == 0) {
            ++m_state->write_iov_cursor;
            continue;
        }

        if (remaining < iov.iov_len) {
            iov.iov_base = static_cast<char*>(iov.iov_base) + remaining;
            iov.iov_len -= remaining;
            return true;
        }

        remaining -= iov.iov_len;
        iov.iov_len = 0;
        ++m_state->write_iov_cursor;
    }

    if (remaining != 0) {
        return false;
    }

    if (m_state->write_iov_cursor >= m_state->write_iovecs.size()) {
        refillWriteIovWindow();
    }

    return true;
}

bool MysqlPipelineAwaitable::Machine::prepareReadWindow()
{
    m_state->read_iov_count = m_state->client->ringBuffer().getWriteIovecs(
        m_state->read_iovecs.data(),
        m_state->read_iovecs.size());
    if (m_state->read_iov_count == 0) {
        setError(MysqlError(
            MYSQL_ERROR_RECV,
            "No writable ring buffer space while receiving pipeline response"));
        return false;
    }
    return true;
}

void MysqlPipelineAwaitable::Machine::resetCurrentResult()
{
    m_state->phase = Phase::ReceivingHeader;
    m_state->current_result = MysqlResultSet{};
    if (m_state->client->asyncConfig().result_row_reserve_hint > 0) {
        m_state->current_result.reserveRows(m_state->client->asyncConfig().result_row_reserve_hint);
    }
    m_state->column_count = 0;
    m_state->columns_received = 0;
}

void MysqlPipelineAwaitable::Machine::finalizeCurrentResult()
{
    m_state->results.push_back(std::move(m_state->current_result));
    if (m_state->results.size() >= m_state->expected_results) {
        m_state->phase = Phase::Done;
    } else {
        resetCurrentResult();
    }
}

galay::kernel::MachineAction<MysqlPipelineAwaitable::Result>
MysqlPipelineAwaitable::Machine::advance()
{
    if (m_state->result.has_value()) {
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    switch (m_state->phase) {
    case Phase::Invalid:
        setError(MysqlError(MYSQL_ERROR_INTERNAL, "Pipeline machine entered invalid state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    case Phase::SendCommands: {
        const size_t pending = pendingWriteIovCount();
        if (pending == 0) {
            m_state->client->ringBuffer().clear();
            m_state->phase = Phase::ReceivingHeader;
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        return galay::kernel::MachineAction<result_type>::waitWritev(
            m_state->write_iovecs.data() + m_state->write_iov_cursor,
            pending);
    }
    case Phase::ReceivingHeader:
    case Phase::ReceivingColumns:
    case Phase::ReceivingColumnEof:
    case Phase::ReceivingRows: {
        auto parsed = tryParseFromRingBuffer();
        if (!parsed.has_value()) {
            setError(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepareReadWindow()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::waitReadv(
            m_state->read_iovecs.data(),
            m_state->read_iov_count);
    }
    case Phase::Done:
        if (!m_state->result.has_value()) {
            m_state->result =
                std::optional<std::vector<MysqlResultSet>>(std::move(m_state->results));
        }
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    setError(MysqlError(MYSQL_ERROR_INTERNAL, "Unknown pipeline machine state"));
    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
}

void MysqlPipelineAwaitable::Machine::onRead(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        setRecvError(result.error());
        return;
    }
    if (result.value() == 0) {
        setError(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, "Connection closed"));
        return;
    }
    m_state->client->ringBuffer().produce(result.value());
}

void MysqlPipelineAwaitable::Machine::onWrite(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result) {
        setSendError(result.error());
        return;
    }

    const size_t sent = result.value();
    if (sent == 0) {
        setError(MysqlError(MYSQL_ERROR_SEND, "Send returned 0 bytes in pipeline"));
        return;
    }

    if (!advanceAfterWrite(sent)) {
        setSendError(IOError(galay::kernel::kSendFailed, 0));
        return;
    }

    if (pendingWriteIovCount() == 0) {
        m_state->client->ringBuffer().clear();
        m_state->phase = Phase::ReceivingHeader;
    }
}
std::expected<bool, MysqlError> MysqlPipelineAwaitable::Machine::tryParseFromRingBuffer()
{
    while (m_state->results.size() < m_state->expected_results) {
        struct iovec read_iovecs[2];
        const size_t read_iovecs_count = m_state->client->ringBuffer().getReadIovecs(read_iovecs, 2);
        if (read_iovecs_count == 0) {
            return false;
        }

        auto linear = detail::linearizeReadIovecs(
            std::span<const struct iovec>(read_iovecs, read_iovecs_count),
            m_state->parse_scratch);
        const char* data = linear.data();
        size_t len = linear.size();

        size_t consumed = 0;
        auto pkt = m_state->client->parser().extractPacket(data, len, consumed);
        if (!pkt) {
            if (pkt.error() == protocol::ParseError::Incomplete) {
                return false;
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse pipeline packet failed"));
        }

        const uint8_t first_byte = static_cast<uint8_t>(pkt->payload[0]);
        const uint32_t caps = m_state->client->serverCapabilities();

        if (m_state->phase == Phase::ReceivingHeader) {
            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parseErr(pkt->payload, pkt->payload_len, caps);
                m_state->client->ringBuffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Pipeline query failed"));
            }

            if (first_byte == 0x00) {
                auto ok = m_state->client->parser().parseOk(pkt->payload, pkt->payload_len, caps);
                m_state->client->ringBuffer().consume(consumed);
                if (!ok) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse OK packet"));
                }

                m_state->current_result.setAffectedRows(ok->affected_rows);
                m_state->current_result.setLastInsertId(ok->last_insert_id);
                m_state->current_result.setWarnings(ok->warnings);
                m_state->current_result.setStatusFlags(ok->status_flags);
                m_state->current_result.setInfo(ok->info);
                finalizeCurrentResult();
                continue;
            }

            size_t int_consumed = 0;
            auto col_count = protocol::readLenEncInt(pkt->payload, pkt->payload_len, int_consumed);
            if (!col_count) {
                m_state->client->ringBuffer().consume(consumed);
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse column count"));
            }

            m_state->column_count = col_count.value();
            m_state->columns_received = 0;
            m_state->current_result.reserveFields(static_cast<size_t>(m_state->column_count));
            m_state->client->ringBuffer().consume(consumed);
            m_state->phase = Phase::ReceivingColumns;
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumns) {
            auto col = m_state->client->parser().parseColumnDefinition(pkt->payload, pkt->payload_len);
            m_state->client->ringBuffer().consume(consumed);
            if (!col) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse column definition"));
            }

            MysqlField field(col->name,
                             static_cast<MysqlFieldType>(col->column_type),
                             col->flags,
                             col->column_length,
                             col->decimals);
            field.setCatalog(col->catalog);
            field.setSchema(col->schema);
            field.setTable(col->table);
            field.setOrgTable(col->org_table);
            field.setOrgName(col->org_name);
            field.setCharacterSet(col->character_set);
            m_state->current_result.addField(std::move(field));

            ++m_state->columns_received;
            if (m_state->columns_received >= m_state->column_count) {
                m_state->phase = (caps & protocol::CLIENT_DEPRECATE_EOF)
                    ? Phase::ReceivingRows
                    : Phase::ReceivingColumnEof;
            }
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumnEof) {
            m_state->client->ringBuffer().consume(consumed);
            m_state->phase = Phase::ReceivingRows;
            continue;
        }

        if (m_state->phase == Phase::ReceivingRows) {
            if (first_byte == 0xFE && pkt->payload_len < 0xFFFFFF) {
                if (caps & protocol::CLIENT_DEPRECATE_EOF) {
                    auto ok = m_state->client->parser().parseOk(pkt->payload, pkt->payload_len, caps);
                    if (ok) {
                        m_state->current_result.setWarnings(ok->warnings);
                        m_state->current_result.setStatusFlags(ok->status_flags);
                    }
                } else {
                    auto eof = m_state->client->parser().parseEof(pkt->payload, pkt->payload_len);
                    if (eof) {
                        m_state->current_result.setWarnings(eof->warnings);
                        m_state->current_result.setStatusFlags(eof->status_flags);
                    }
                }

                m_state->client->ringBuffer().consume(consumed);
                finalizeCurrentResult();
                continue;
            }

            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parseErr(pkt->payload, pkt->payload_len, caps);
                m_state->client->ringBuffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Pipeline row fetch failed"));
            }

            auto row = m_state->client->parser().parseTextRow(
                pkt->payload,
                pkt->payload_len,
                m_state->column_count);
            m_state->client->ringBuffer().consume(consumed);
            if (!row) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse text row"));
            }
            m_state->current_result.addRow(MysqlRow(std::move(row.value())));
            continue;
        }

        return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Invalid pipeline parser state"));
    }

    m_state->phase = Phase::Done;
    return true;
}

// ======================== AsyncMysqlClient 实现 ========================

AsyncMysqlClient::AsyncMysqlClient(IOScheduler* scheduler,
                                   AsyncMysqlConfig config)
    : m_scheduler(scheduler)
    , m_config(std::move(config))
    , m_ring_buffer(m_config.buffer_size)
{
}

AsyncMysqlClient::AsyncMysqlClient(AsyncMysqlClient&& other) noexcept
    : m_is_closed(other.m_is_closed)
    , m_socket(std::move(other.m_socket))
    , m_scheduler(other.m_scheduler)
    , m_parser(std::move(other.m_parser))
    , m_encoder(std::move(other.m_encoder))
    , m_config(std::move(other.m_config))
    , m_ring_buffer(std::move(other.m_ring_buffer))
    , m_server_capabilities(other.m_server_capabilities)
{
    other.m_is_closed = true;
}

AsyncMysqlClient& AsyncMysqlClient::operator=(AsyncMysqlClient&& other) noexcept
{
    if (this != &other) {
        m_is_closed = other.m_is_closed;
        m_socket = std::move(other.m_socket);
        m_scheduler = other.m_scheduler;
        m_parser = std::move(other.m_parser);
        m_encoder = std::move(other.m_encoder);
        m_config = std::move(other.m_config);
        m_ring_buffer = std::move(other.m_ring_buffer);
        m_server_capabilities = other.m_server_capabilities;
        other.m_is_closed = true;
    }
    return *this;
}

MysqlConnectAwaitable AsyncMysqlClient::connect(MysqlConfig config)
{
    return MysqlConnectAwaitable(*this, std::move(config));
}

MysqlConnectAwaitable AsyncMysqlClient::connect(std::string_view host, uint16_t port,
                                                std::string_view user, std::string_view password,
                                                std::string_view database)
{
    MysqlConfig config;
    config.host.assign(host.data(), host.size());
    config.port = port;
    config.username.assign(user.data(), user.size());
    config.password.assign(password.data(), password.size());
    config.database.assign(database.data(), database.size());
    return connect(std::move(config));
}

MysqlQueryAwaitable AsyncMysqlClient::query(std::string_view sql)
{
    return MysqlQueryAwaitable(*this, sql);
}

MysqlPipelineAwaitable AsyncMysqlClient::batch(std::span<const protocol::MysqlCommandView> commands)
{
    return MysqlPipelineAwaitable(*this, commands);
}

MysqlPipelineAwaitable AsyncMysqlClient::pipeline(std::span<const std::string_view> sqls)
{
    size_t reserve_bytes = 0;
    for (const auto sql : sqls) {
        reserve_bytes += protocol::MYSQL_PACKET_HEADER_SIZE + 1 + sql.size();
    }

    protocol::MysqlCommandBuilder builder;
    builder.reserve(sqls.size(), reserve_bytes);
    for (const auto sql : sqls) {
        builder.appendQuery(sql);
    }

    return batch(builder.commands());
}

MysqlPrepareAwaitable AsyncMysqlClient::prepare(std::string_view sql)
{
    return MysqlPrepareAwaitable(*this, sql);
}

MysqlStmtExecuteAwaitable AsyncMysqlClient::stmtExecute(uint32_t stmt_id,
                                                        std::span<const std::optional<std::string>> params,
                                                        std::span<const uint8_t> param_types)
{
    return MysqlStmtExecuteAwaitable(*this, m_encoder.encodeStmtExecute(stmt_id, params, param_types, 0));
}

MysqlStmtExecuteAwaitable AsyncMysqlClient::stmtExecute(uint32_t stmt_id,
                                                        std::span<const std::optional<std::string_view>> params,
                                                        std::span<const uint8_t> param_types)
{
    return MysqlStmtExecuteAwaitable(*this, m_encoder.encodeStmtExecute(stmt_id, params, param_types, 0));
}

MysqlQueryAwaitable AsyncMysqlClient::beginTransaction()
{
    return query("BEGIN");
}

MysqlQueryAwaitable AsyncMysqlClient::commit()
{
    return query("COMMIT");
}

MysqlQueryAwaitable AsyncMysqlClient::rollback()
{
    return query("ROLLBACK");
}

MysqlQueryAwaitable AsyncMysqlClient::ping()
{
    return query("SELECT 1");
}

MysqlQueryAwaitable AsyncMysqlClient::useDatabase(std::string_view database)
{
    std::string sql;
    sql.reserve(4 + database.size());
    sql.append("USE ");
    sql.append(database.data(), database.size());
    return query(sql);
}

} // namespace galay::mysql
