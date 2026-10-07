#ifndef GALAY_MYSQL_DETAILS_AWAITABLE_INL
#define GALAY_MYSQL_DETAILS_AWAITABLE_INL

#include "../base/mysql_log.h"
#include "../protoc/builder.h"
#include <array>
#include <concepts>
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

inline void sync_send_window(const std::string& payload, size_t sent, const char*& buffer, size_t& length)
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
bool handle_send_result(std::expected<size_t, IOError>& io_result,
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

template<RingBufferBackendStrategy Strategy>
bool prepare_recv_window(RingBuffer<Strategy>& ring_buffer, std::vector<struct iovec>& iovecs)
{
    struct iovec raw_iovecs[2];
    const size_t count = ring_buffer.get_write_iovecs(raw_iovecs, 2);
    iovecs.assign(raw_iovecs, raw_iovecs + count);
    return !iovecs.empty();
}

template<typename ParseFnType, typename OnParseError>
requires ParseFn<ParseFnType> &&
         ParseErrorCallback<OnParseError>
bool parse_or_set_error(ParseFnType&& parse_fn, OnParseError&& on_parse_error)
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
bool handle_read_result(std::expected<size_t, IOError>& io_result,
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
    return detail::parse_or_set_error(std::forward<ParseFnType>(parse_fn),
                                   std::forward<OnParseError>(on_parse_error));
}

MysqlError to_timeout_or_internal_error(const IOError& io_error)
{
    if (IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
        return MysqlError(MYSQL_ERROR_TIMEOUT, io_error.message());
    }
    return MysqlError(MYSQL_ERROR_INTERNAL, io_error.message());
}

MysqlError map_awaitable_io_error(const IOError& io_error, MysqlErrorType fallback_type)
{
    if (IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
        return MysqlError(MYSQL_ERROR_TIMEOUT, io_error.message());
    }
    if (IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
        return MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, io_error.message());
    }
    return MysqlError(fallback_type, io_error.message());
}

inline std::string_view linearize_read_iovecs(std::span<const struct iovec> iovecs, std::string& scratch)
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

inline std::string build_single_command_packet(protocol::CommandType cmd,
                                            std::string_view payload,
                                            protocol::MysqlCommandKind kind)
{
    protocol::MysqlCommandBuilder builder;
    builder.reserve(1, protocol::MYSQL_PACKET_HEADER_SIZE + 1 + payload.size());
    builder.append_fast(cmd, payload, 0, kind);
    return std::move(builder.release().encoded);
}

inline std::string encode_raw_packet(std::string_view payload, uint8_t sequence_id)
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

std::array<struct iovec, 1>& empty_iovecs()
{
    static std::array<struct iovec, 1> empty{};
    return empty;
}

}

namespace details
{

// ======================== MysqlConnectAwaitable<Strategy> ========================

template<RingBufferBackendStrategy Strategy>
MysqlConnectAwaitable<Strategy>::MysqlConnectAwaitable(AsyncMysqlClient<Strategy>& client, MysqlConfig config)
    : m_state(std::make_shared<SharedState>(client, std::move(config)))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::from_state_machine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

template<RingBufferBackendStrategy Strategy>
bool MysqlConnectAwaitable<Strategy>::is_invalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
MysqlConnectAwaitable<Strategy>::SharedState::SharedState(AsyncMysqlClient<Strategy>& client, MysqlConfig config_in)
    : client(&client)
    , config(std::move(config_in))
    , host(IPType::IPV4, config.host, config.port)
{
    client.ring_buffer().clear();
    auto nonblock_result = client.socket().option().handle_non_block();
    if (!nonblock_result) {
        result = std::unexpected(MysqlError(
            MYSQL_ERROR_CONNECTION,
            "Failed to set non-blocking before connect: " + nonblock_result.error().message()));
        phase = Phase::Invalid;
        return;
    }

    if (config.tcp_no_delay) {
        auto nodelay_result = client.socket().option().handle_tcp_no_delay();
        if (!nodelay_result) {
            result = std::unexpected(MysqlError(
                MYSQL_ERROR_CONNECTION,
                "Failed to set TCP_NODELAY before connect: " + nodelay_result.error().message()));
            phase = Phase::Invalid;
            return;
        }
    }
}

template<RingBufferBackendStrategy Strategy>
MysqlConnectAwaitable<Strategy>::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

template<RingBufferBackendStrategy Strategy>
void MysqlConnectAwaitable<Strategy>::Machine::set_error(MysqlError error) noexcept
{
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
void MysqlConnectAwaitable<Strategy>::Machine::set_connect_error(const IOError& io_error) noexcept
{
    set_error(MysqlError(MYSQL_ERROR_CONNECTION, io_error.message()));
}

template<RingBufferBackendStrategy Strategy>
void MysqlConnectAwaitable<Strategy>::Machine::set_send_error(const IOError& io_error) noexcept
{
    set_error(MysqlError(MYSQL_ERROR_SEND, io_error.message()));
}

template<RingBufferBackendStrategy Strategy>
void MysqlConnectAwaitable<Strategy>::Machine::set_recv_error(const std::string& phase, const IOError& io_error) noexcept
{
    set_error(MysqlError(MYSQL_ERROR_RECV, io_error.message() + " during " + phase));
}

template<RingBufferBackendStrategy Strategy>
void MysqlConnectAwaitable<Strategy>::Machine::complete_success() noexcept
{
    m_state->connected = true;
    m_state->phase = Phase::Done;
    m_state->result = std::optional<bool>(true);
    MYSQL_LOG_INFO("[client]", "MySQL connected successfully to {}:{}",
                 m_state->config.host,
                 m_state->config.port);
}

template<RingBufferBackendStrategy Strategy>
bool MysqlConnectAwaitable<Strategy>::Machine::prepare_read_window()
{
    m_state->read_iov_count = m_state->client->ring_buffer().get_write_iovecs(
        m_state->read_iovecs.data(),
        m_state->read_iovecs.size());
    if (m_state->read_iov_count == 0) {
        const char* phase_name = m_state->phase == Phase::HandshakeRead ? "handshake" : "auth";
        set_error(MysqlError(MYSQL_ERROR_RECV,
                            std::string("No writable ring buffer space while waiting ") + phase_name));
        return false;
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::MachineAction<typename MysqlConnectAwaitable<Strategy>::Result>
MysqlConnectAwaitable<Strategy>::Machine::advance()
{
    if (m_state->result.has_value()) {
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    switch (m_state->phase) {
    case Phase::Invalid:
        set_error(MysqlError(MYSQL_ERROR_INTERNAL, "Connect machine entered invalid state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    case Phase::Connect:
        return galay::kernel::MachineAction<result_type>::wait_connect(m_state->host);
    case Phase::HandshakeRead: {
        auto parsed = parse_handshake_from_ring_buffer();
        if (!parsed.has_value()) {
            set_error(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepare_read_window()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::wait_readv(
            m_state->read_iovecs.data(),
            m_state->read_iov_count);
    }
    case Phase::AuthWrite:
        if (m_state->sent >= m_state->auth_packet.size()) {
            m_state->sent = 0;
            m_state->phase = Phase::AuthResultRead;
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        return galay::kernel::MachineAction<result_type>::wait_write(
            m_state->auth_packet.data() + m_state->sent,
            m_state->auth_packet.size() - m_state->sent);
    case Phase::AuthResultRead: {
        auto parsed = parse_auth_result_from_ring_buffer();
        if (!parsed.has_value()) {
            set_error(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepare_read_window()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::wait_readv(
            m_state->read_iovecs.data(),
            m_state->read_iov_count);
    }
    case Phase::Done:
        if (!m_state->result.has_value()) {
            complete_success();
        }
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    set_error(MysqlError(MYSQL_ERROR_INTERNAL, "Unknown connect machine state"));
    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
}

template<RingBufferBackendStrategy Strategy>
void MysqlConnectAwaitable<Strategy>::Machine::on_connect(std::expected<void, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        set_connect_error(result.error());
        return;
    }

    auto nonblock_result = m_state->client->socket().option().handle_non_block();
    if (!nonblock_result) {
        set_error(MysqlError(MYSQL_ERROR_CONNECTION,
                            "Failed to keep non-blocking after connect: " +
                                nonblock_result.error().message()));
        return;
    }

    m_state->phase = Phase::HandshakeRead;
}

template<RingBufferBackendStrategy Strategy>
void MysqlConnectAwaitable<Strategy>::Machine::on_read(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }

    const char* phase_name = m_state->phase == Phase::HandshakeRead ? "handshake" : "auth";
    if (!result.has_value()) {
        set_recv_error(phase_name, result.error());
        return;
    }

    const size_t n = result.value();
    if (n == 0) {
        set_error(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED,
                            std::string("Connection closed during ") + phase_name));
        return;
    }

    m_state->client->ring_buffer().produce(n);

    auto parsed = m_state->phase == Phase::HandshakeRead
        ? parse_handshake_from_ring_buffer()
        : parse_auth_result_from_ring_buffer();
    if (!parsed.has_value()) {
        set_error(std::move(parsed.error()));
    }
}

template<RingBufferBackendStrategy Strategy>
void MysqlConnectAwaitable<Strategy>::Machine::on_write(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        set_send_error(result.error());
        return;
    }

    const size_t sent_once = result.value();
    if (sent_once == 0) {
        set_error(MysqlError(MYSQL_ERROR_SEND, "Send returned 0 bytes"));
        return;
    }

    m_state->sent += sent_once;
    if (m_state->sent >= m_state->auth_packet.size()) {
        m_state->sent = 0;
        m_state->phase = Phase::AuthResultRead;
    }
}

template<RingBufferBackendStrategy Strategy>
std::expected<bool, MysqlError> MysqlConnectAwaitable<Strategy>::Machine::parse_handshake_from_ring_buffer()
{
    struct iovec read_iovecs[2];
    const size_t read_iovecs_count = m_state->client->ring_buffer().get_read_iovecs(read_iovecs, 2);
    if (read_iovecs_count == 0) {
        return false;
    }

    auto linear = detail::linearize_read_iovecs(
        std::span<const struct iovec>(read_iovecs, read_iovecs_count),
        m_state->parse_scratch);
    const char* data = linear.data();
    size_t len = linear.size();

    size_t consumed = 0;
    auto pkt = m_state->client->parser().extract_packet(data, len, consumed);
    if (!pkt) {
        if (pkt.error() == protocol::ParseError::Incomplete) {
            return false;
        }
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse handshake packet"));
    }

    if (static_cast<uint8_t>(pkt->payload[0]) == 0xFF) {
        auto err = m_state->client->parser().parse_err(
            pkt->payload,
            pkt->payload_len,
            protocol::CLIENT_PROTOCOL_41);
        m_state->client->ring_buffer().consume(consumed);
        if (err) {
            return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
        }
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse handshake ERR packet"));
    }

    auto hs = m_state->client->parser().parse_handshake(pkt->payload, pkt->payload_len);
    m_state->client->ring_buffer().consume(consumed);
    if (!hs) {
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse handshake packet body"));
    }
    m_state->handshake = std::move(hs.value());
    m_state->auth_plugin_name = m_state->handshake.auth_plugin_name;
    m_state->auth_plugin_data = m_state->handshake.auth_plugin_data;

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
    m_state->client->set_server_capabilities(resp.capability_flags);
    resp.character_set = protocol::CHARSET_UTF8MB4_GENERAL_CI;
    resp.username = m_state->config.username;
    resp.database = m_state->config.database;
    resp.auth_plugin_name = m_state->auth_plugin_name;

    auto initial_auth = protocol::AuthPlugin::auth_response_for_plugin(
        m_state->auth_plugin_name,
        m_state->config.password,
        m_state->auth_plugin_data);
    if (initial_auth) {
        resp.auth_response = std::move(initial_auth.value());
    } else {
        resp.auth_response = protocol::AuthPlugin::native_password_auth(
            m_state->config.password,
            m_state->handshake.auth_plugin_data);
        resp.auth_plugin_name = "mysql_native_password";
        m_state->auth_plugin_name = resp.auth_plugin_name;
    }

    m_state->auth_stage = AuthStage::InitialResponse;
    m_state->auth_packet =
        m_state->client->encoder().encode_handshake_response(resp, pkt->sequence_id + 1);
    m_state->sent = 0;
    m_state->phase = Phase::AuthWrite;
    return true;
}

template<RingBufferBackendStrategy Strategy>
std::expected<bool, MysqlError> MysqlConnectAwaitable<Strategy>::Machine::parse_auth_result_from_ring_buffer()
{
    while (true) {
        struct iovec read_iovecs[2];
        const size_t read_iovecs_count = m_state->client->ring_buffer().get_read_iovecs(read_iovecs, 2);
        if (read_iovecs_count == 0) {
            return false;
        }

        auto linear = detail::linearize_read_iovecs(
            std::span<const struct iovec>(read_iovecs, read_iovecs_count),
            m_state->parse_scratch);
        const char* data = linear.data();
        size_t len = linear.size();

        size_t consumed = 0;
        auto pkt = m_state->client->parser().extract_packet(data, len, consumed);
        if (!pkt) {
            if (pkt.error() == protocol::ParseError::Incomplete) {
                return false;
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse auth response packet"));
        }

        const uint8_t first_byte = static_cast<uint8_t>(pkt->payload[0]);
        m_state->client->ring_buffer().consume(consumed);

        if (m_state->auth_stage == AuthStage::AwaitPublicKey) {
            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parse_err(
                    pkt->payload,
                    pkt->payload_len,
                    m_state->client->server_capabilities());
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Public key request failed"));
            }

            std::string_view public_key(pkt->payload, pkt->payload_len);
            if (!public_key.empty() && static_cast<uint8_t>(public_key.front()) == 0x01) {
                public_key.remove_prefix(1);
            }

            auto encrypted = protocol::AuthPlugin::caching_sha2_full_auth(
                m_state->config.password,
                m_state->auth_plugin_data,
                public_key);
            if (!encrypted) {
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, encrypted.error()));
            }

            m_state->auth_packet = detail::encode_raw_packet(*encrypted, pkt->sequence_id + 1);
            m_state->sent = 0;
            m_state->auth_stage = AuthStage::AwaitFinalResult;
            m_state->phase = Phase::AuthWrite;
            return true;
        }

        if (first_byte == 0x00) {
            complete_success();
            return true;
        }

        if (first_byte == 0xFF) {
            auto err = m_state->client->parser().parse_err(
                pkt->payload,
                pkt->payload_len,
                m_state->client->server_capabilities());
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
                m_state->auth_plugin_name == "caching_sha2_password") {
                static const std::string kPublicKeyRequest(1, '\x02');
                m_state->auth_packet = detail::encode_raw_packet(kPublicKeyRequest, pkt->sequence_id + 1);
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
            auto auth_switch = m_state->client->parser().parse_auth_switch_request(
                pkt->payload,
                pkt->payload_len);
            if (!auth_switch) {
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Failed to parse auth switch request"));
            }

            m_state->auth_plugin_name = std::move(auth_switch->auth_plugin_name);
            m_state->auth_plugin_data = std::move(auth_switch->auth_plugin_data);
            auto switched_auth = protocol::AuthPlugin::auth_response_for_plugin(
                m_state->auth_plugin_name,
                m_state->config.password,
                m_state->auth_plugin_data);
            if (!switched_auth) {
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, switched_auth.error()));
            }

            m_state->auth_packet = detail::encode_raw_packet(*switched_auth, pkt->sequence_id + 1);
            m_state->sent = 0;
            m_state->auth_stage = AuthStage::AwaitFinalResult;
            m_state->phase = Phase::AuthWrite;
            return true;
        }

        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Unexpected auth response packet"));
    }
}

// ======================== MysqlQueryAwaitable<Strategy> ========================

template<RingBufferBackendStrategy Strategy>
MysqlQueryAwaitable<Strategy>::MysqlQueryAwaitable(AsyncMysqlClient<Strategy>& client, std::string_view sql)
    : m_state(std::make_shared<SharedState>(client, sql))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::from_state_machine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

template<RingBufferBackendStrategy Strategy>
bool MysqlQueryAwaitable<Strategy>::is_invalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
MysqlQueryAwaitable<Strategy>::SharedState::SharedState(AsyncMysqlClient<Strategy>& client, std::string_view sql)
    : client(&client)
    , encoded_cmd(detail::build_single_command_packet(protocol::CommandType::COM_QUERY,
                                                   sql,
                                                   protocol::MysqlCommandKind::Query))
{
    if (encoded_cmd.empty()) {
        result = std::unexpected(MysqlError(MYSQL_ERROR_INVALID_PARAM,
                                            "MySQL query exceeds single-packet limit"));
        phase = Phase::Invalid;
        return;
    }

    if (client.async_config().result_row_reserve_hint > 0) {
        result_set.reserve_rows(client.async_config().result_row_reserve_hint);
    }
}

template<RingBufferBackendStrategy Strategy>
MysqlQueryAwaitable<Strategy>::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

template<RingBufferBackendStrategy Strategy>
void MysqlQueryAwaitable<Strategy>::Machine::set_error(MysqlError error) noexcept
{
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
void MysqlQueryAwaitable<Strategy>::Machine::set_send_error(const IOError& io_error) noexcept
{
    MYSQL_LOG_DEBUG("[client]", "send query failed: {}", io_error.message());
    set_error(detail::map_awaitable_io_error(io_error, MYSQL_ERROR_SEND));
}

template<RingBufferBackendStrategy Strategy>
void MysqlQueryAwaitable<Strategy>::Machine::set_recv_error(const IOError& io_error) noexcept
{
    MYSQL_LOG_DEBUG("[client]", "recv query failed: {}", io_error.message());
    set_error(detail::map_awaitable_io_error(io_error, MYSQL_ERROR_RECV));
}

template<RingBufferBackendStrategy Strategy>
bool MysqlQueryAwaitable<Strategy>::Machine::prepare_read_window()
{
    m_state->read_iov_count = m_state->client->ring_buffer().get_write_iovecs(
        m_state->read_iovecs.data(),
        m_state->read_iovecs.size());
    if (m_state->read_iov_count == 0) {
        set_error(MysqlError(MYSQL_ERROR_RECV, "No writable ring buffer space"));
        return false;
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::MachineAction<typename MysqlQueryAwaitable<Strategy>::Result>
MysqlQueryAwaitable<Strategy>::Machine::advance()
{
    if (m_state->result.has_value()) {
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    switch (m_state->phase) {
    case Phase::Invalid:
        set_error(MysqlError(MYSQL_ERROR_INTERNAL, "Query machine entered invalid state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    case Phase::SendCommand:
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->client->ring_buffer().clear();
            m_state->phase = Phase::ReceivingHeader;
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        return galay::kernel::MachineAction<result_type>::wait_write(
            m_state->encoded_cmd.data() + m_state->sent,
            m_state->encoded_cmd.size() - m_state->sent);
    case Phase::ReceivingHeader:
    case Phase::ReceivingColumns:
    case Phase::ReceivingColumnEof:
    case Phase::ReceivingRows: {
        auto parsed = try_parse_from_ring_buffer();
        if (!parsed.has_value()) {
            set_error(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepare_read_window()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::wait_readv(
            m_state->read_iovecs.data(),
            m_state->read_iov_count);
    }
    case Phase::Done:
        if (!m_state->result.has_value()) {
            m_state->result = std::optional<MysqlResultSet>(std::move(m_state->result_set));
        }
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    set_error(MysqlError(MYSQL_ERROR_INTERNAL, "Unknown query machine state"));
    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
}

template<RingBufferBackendStrategy Strategy>
void MysqlQueryAwaitable<Strategy>::Machine::on_read(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        set_recv_error(result.error());
        return;
    }
    if (result.value() == 0) {
        set_error(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, "Connection closed"));
        return;
    }
    m_state->client->ring_buffer().produce(result.value());
}

template<RingBufferBackendStrategy Strategy>
void MysqlQueryAwaitable<Strategy>::Machine::on_write(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        set_send_error(result.error());
        return;
    }
    if (result.value() == 0) {
        set_error(MysqlError(MYSQL_ERROR_SEND, "Send returned 0 bytes"));
        return;
    }
    m_state->sent += result.value();
    if (m_state->sent >= m_state->encoded_cmd.size()) {
        m_state->client->ring_buffer().clear();
        m_state->phase = Phase::ReceivingHeader;
    }
}

template<RingBufferBackendStrategy Strategy>
std::expected<bool, MysqlError> MysqlQueryAwaitable<Strategy>::Machine::try_parse_from_ring_buffer()
{
    while (true) {
        struct iovec read_iovecs[2];
        const size_t read_iovecs_count = m_state->client->ring_buffer().get_read_iovecs(read_iovecs, 2);
        if (read_iovecs_count == 0) {
            return false;
        }

        auto linear = detail::linearize_read_iovecs(
            std::span<const struct iovec>(read_iovecs, read_iovecs_count),
            m_state->parse_scratch);
        const char* data = linear.data();
        size_t len = linear.size();

        size_t consumed = 0;
        auto pkt = m_state->client->parser().extract_packet(data, len, consumed);
        if (!pkt) {
            if (pkt.error() == protocol::ParseError::Incomplete) {
                return false;
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse query packet failed"));
        }

        const uint8_t first_byte = static_cast<uint8_t>(pkt->payload[0]);
        const uint32_t caps = m_state->client->server_capabilities();

        if (m_state->phase == Phase::ReceivingHeader) {
            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parse_err(pkt->payload, pkt->payload_len, caps);
                m_state->client->ring_buffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Query failed"));
            }

            if (first_byte == 0x00) {
                auto ok = m_state->client->parser().parse_ok(pkt->payload, pkt->payload_len, caps);
                m_state->client->ring_buffer().consume(consumed);
                if (!ok) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse OK packet"));
                }
                m_state->result_set.set_affected_rows(ok->affected_rows);
                m_state->result_set.set_last_insert_id(ok->last_insert_id);
                m_state->result_set.set_warnings(ok->warnings);
                m_state->result_set.set_status_flags(ok->status_flags);
                m_state->result_set.set_info(ok->info);
                m_state->phase = Phase::Done;
                return true;
            }

            size_t int_consumed = 0;
            auto col_count = protocol::read_len_enc_int(pkt->payload, pkt->payload_len, int_consumed);
            if (!col_count) {
                m_state->client->ring_buffer().consume(consumed);
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse column count"));
            }

            m_state->column_count = col_count.value();
            m_state->columns_received = 0;
            m_state->result_set.reserve_fields(static_cast<size_t>(m_state->column_count));
            m_state->client->ring_buffer().consume(consumed);
            m_state->phase = Phase::ReceivingColumns;
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumns) {
            auto col = m_state->client->parser().parse_column_definition(pkt->payload, pkt->payload_len);
            m_state->client->ring_buffer().consume(consumed);
            if (!col) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse column definition"));
            }

            MysqlField field(col->name,
                             static_cast<MysqlFieldType>(col->column_type),
                             col->flags,
                             col->column_length,
                             col->decimals);
            field.set_catalog(col->catalog);
            field.set_schema(col->schema);
            field.set_table(col->table);
            field.set_org_table(col->org_table);
            field.set_org_name(col->org_name);
            field.set_character_set(col->character_set);
            m_state->result_set.add_field(std::move(field));

            ++m_state->columns_received;
            if (m_state->columns_received >= m_state->column_count) {
                m_state->phase = (caps & protocol::CLIENT_DEPRECATE_EOF)
                    ? Phase::ReceivingRows
                    : Phase::ReceivingColumnEof;
            }
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumnEof) {
            m_state->client->ring_buffer().consume(consumed);
            m_state->phase = Phase::ReceivingRows;
            continue;
        }

        if (m_state->phase == Phase::ReceivingRows) {
            if (first_byte == 0xFE && pkt->payload_len < 0xFFFFFF) {
                if (caps & protocol::CLIENT_DEPRECATE_EOF) {
                    auto ok = m_state->client->parser().parse_ok(pkt->payload, pkt->payload_len, caps);
                    if (ok) {
                        m_state->result_set.set_warnings(ok->warnings);
                        m_state->result_set.set_status_flags(ok->status_flags);
                    }
                } else {
                    auto eof = m_state->client->parser().parse_eof(pkt->payload, pkt->payload_len);
                    if (eof) {
                        m_state->result_set.set_warnings(eof->warnings);
                        m_state->result_set.set_status_flags(eof->status_flags);
                    }
                }
                m_state->client->ring_buffer().consume(consumed);
                m_state->phase = Phase::Done;
                return true;
            }

            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parse_err(pkt->payload, pkt->payload_len, caps);
                m_state->client->ring_buffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Error during row fetch"));
            }

            auto row = m_state->client->parser().parse_text_row(
                pkt->payload,
                pkt->payload_len,
                m_state->column_count);
            m_state->client->ring_buffer().consume(consumed);
            if (!row) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse text row"));
            }
            m_state->result_set.add_row(MysqlRow(std::move(row.value())));
            continue;
        }

        return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Invalid query parser state"));
    }
}

// ======================== MysqlPrepareAwaitable<Strategy> ========================

template<RingBufferBackendStrategy Strategy>
MysqlPrepareAwaitable<Strategy>::MysqlPrepareAwaitable(AsyncMysqlClient<Strategy>& client, std::string_view sql)
    : m_state(std::make_shared<SharedState>(client, sql))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::from_state_machine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

template<RingBufferBackendStrategy Strategy>
bool MysqlPrepareAwaitable<Strategy>::is_invalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
MysqlPrepareAwaitable<Strategy>::SharedState::SharedState(AsyncMysqlClient<Strategy>& client, std::string_view sql)
    : client(&client)
    , encoded_cmd(detail::build_single_command_packet(protocol::CommandType::COM_STMT_PREPARE,
                                                   sql,
                                                   protocol::MysqlCommandKind::StmtPrepare))
{
    if (encoded_cmd.empty()) {
        result = std::unexpected(MysqlError(MYSQL_ERROR_INVALID_PARAM,
                                            "MySQL prepare exceeds single-packet limit"));
        phase = Phase::Invalid;
    }
}

template<RingBufferBackendStrategy Strategy>
MysqlPrepareAwaitable<Strategy>::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

template<RingBufferBackendStrategy Strategy>
void MysqlPrepareAwaitable<Strategy>::Machine::set_error(MysqlError error) noexcept
{
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
void MysqlPrepareAwaitable<Strategy>::Machine::set_send_error(const IOError& io_error) noexcept
{
    set_error(detail::map_awaitable_io_error(io_error, MYSQL_ERROR_SEND));
}

template<RingBufferBackendStrategy Strategy>
void MysqlPrepareAwaitable<Strategy>::Machine::set_recv_error(const IOError& io_error) noexcept
{
    set_error(detail::map_awaitable_io_error(io_error, MYSQL_ERROR_RECV));
}

template<RingBufferBackendStrategy Strategy>
bool MysqlPrepareAwaitable<Strategy>::Machine::prepare_read_window()
{
    m_state->read_iov_count = m_state->client->ring_buffer().get_write_iovecs(
        m_state->read_iovecs.data(),
        m_state->read_iovecs.size());
    if (m_state->read_iov_count == 0) {
        set_error(MysqlError(MYSQL_ERROR_RECV, "No writable ring buffer space"));
        return false;
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::MachineAction<typename MysqlPrepareAwaitable<Strategy>::Result>
MysqlPrepareAwaitable<Strategy>::Machine::advance()
{
    if (m_state->result.has_value()) {
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    switch (m_state->phase) {
    case Phase::Invalid:
        set_error(MysqlError(MYSQL_ERROR_INTERNAL, "Prepare machine entered invalid state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    case Phase::SendCommand:
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->client->ring_buffer().clear();
            m_state->phase = Phase::ReceivingPrepareOk;
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        return galay::kernel::MachineAction<result_type>::wait_write(
            m_state->encoded_cmd.data() + m_state->sent,
            m_state->encoded_cmd.size() - m_state->sent);
    case Phase::ReceivingPrepareOk:
    case Phase::ReceivingParamDefs:
    case Phase::ReceivingParamEof:
    case Phase::ReceivingColumnDefs:
    case Phase::ReceivingColumnEof: {
        auto parsed = try_parse_from_ring_buffer();
        if (!parsed.has_value()) {
            set_error(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepare_read_window()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::wait_readv(
            m_state->read_iovecs.data(),
            m_state->read_iov_count);
    }
    case Phase::Done:
        if (!m_state->result.has_value()) {
            m_state->result = std::optional<PrepareResult>(std::move(m_state->prepare_result));
        }
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    set_error(MysqlError(MYSQL_ERROR_INTERNAL, "Unknown prepare machine state"));
    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
}

template<RingBufferBackendStrategy Strategy>
void MysqlPrepareAwaitable<Strategy>::Machine::on_read(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        set_recv_error(result.error());
        return;
    }
    if (result.value() == 0) {
        set_error(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, "Connection closed"));
        return;
    }
    m_state->client->ring_buffer().produce(result.value());
}

template<RingBufferBackendStrategy Strategy>
void MysqlPrepareAwaitable<Strategy>::Machine::on_write(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        set_send_error(result.error());
        return;
    }
    if (result.value() == 0) {
        set_error(MysqlError(MYSQL_ERROR_SEND, "Send returned 0 bytes"));
        return;
    }
    m_state->sent += result.value();
    if (m_state->sent >= m_state->encoded_cmd.size()) {
        m_state->client->ring_buffer().clear();
        m_state->phase = Phase::ReceivingPrepareOk;
    }
}

template<RingBufferBackendStrategy Strategy>
std::expected<bool, MysqlError> MysqlPrepareAwaitable<Strategy>::Machine::try_parse_from_ring_buffer()
{
    while (true) {
        struct iovec read_iovecs[2];
        const size_t read_iovecs_count = m_state->client->ring_buffer().get_read_iovecs(read_iovecs, 2);
        if (read_iovecs_count == 0) {
            return false;
        }

        auto linear = detail::linearize_read_iovecs(
            std::span<const struct iovec>(read_iovecs, read_iovecs_count),
            m_state->parse_scratch);
        const char* data = linear.data();
        size_t len = linear.size();

        size_t consumed = 0;
        auto pkt = m_state->client->parser().extract_packet(data, len, consumed);
        if (!pkt) {
            if (pkt.error() == protocol::ParseError::Incomplete) {
                return false;
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse prepare packet failed"));
        }

        const uint8_t first_byte = static_cast<uint8_t>(pkt->payload[0]);
        const uint32_t caps = m_state->client->server_capabilities();

        if (m_state->phase == Phase::ReceivingPrepareOk) {
            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parse_err(pkt->payload, pkt->payload_len, caps);
                m_state->client->ring_buffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_PREPARED_STMT, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_PREPARED_STMT, "Prepare failed"));
            }

            auto ok = m_state->client->parser().parse_stmt_prepare_ok(pkt->payload, pkt->payload_len);
            m_state->client->ring_buffer().consume(consumed);
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
            auto col = m_state->client->parser().parse_column_definition(pkt->payload, pkt->payload_len);
            m_state->client->ring_buffer().consume(consumed);
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
            m_state->client->ring_buffer().consume(consumed);
            if (m_state->prepare_result.num_columns > 0) {
                m_state->phase = Phase::ReceivingColumnDefs;
                continue;
            }
            m_state->phase = Phase::Done;
            return true;
        }

        if (m_state->phase == Phase::ReceivingColumnDefs) {
            auto col = m_state->client->parser().parse_column_definition(pkt->payload, pkt->payload_len);
            m_state->client->ring_buffer().consume(consumed);
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
            m_state->client->ring_buffer().consume(consumed);
            m_state->phase = Phase::Done;
            return true;
        }

        return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Invalid prepare parser state"));
    }
}

// ======================== MysqlStmtExecuteAwaitable<Strategy> ========================

template<RingBufferBackendStrategy Strategy>
MysqlStmtExecuteAwaitable<Strategy>::MysqlStmtExecuteAwaitable(AsyncMysqlClient<Strategy>& client, std::string encoded_cmd)
    : m_state(std::make_shared<SharedState>(client, std::move(encoded_cmd)))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::from_state_machine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

template<RingBufferBackendStrategy Strategy>
bool MysqlStmtExecuteAwaitable<Strategy>::is_invalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
MysqlStmtExecuteAwaitable<Strategy>::SharedState::SharedState(AsyncMysqlClient<Strategy>& client, std::string encoded_cmd_in)
    : client(&client)
    , encoded_cmd(std::move(encoded_cmd_in))
{
    if (encoded_cmd.empty()) {
        result = std::unexpected(MysqlError(MYSQL_ERROR_INVALID_PARAM,
                                            "MySQL statement execute exceeds single-packet limit"));
        phase = Phase::Invalid;
        return;
    }

    if (client.async_config().result_row_reserve_hint > 0) {
        result_set.reserve_rows(client.async_config().result_row_reserve_hint);
    }
}

template<RingBufferBackendStrategy Strategy>
MysqlStmtExecuteAwaitable<Strategy>::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

template<RingBufferBackendStrategy Strategy>
void MysqlStmtExecuteAwaitable<Strategy>::Machine::set_error(MysqlError error) noexcept
{
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
void MysqlStmtExecuteAwaitable<Strategy>::Machine::set_send_error(const IOError& io_error) noexcept
{
    set_error(detail::map_awaitable_io_error(io_error, MYSQL_ERROR_SEND));
}

template<RingBufferBackendStrategy Strategy>
void MysqlStmtExecuteAwaitable<Strategy>::Machine::set_recv_error(const IOError& io_error) noexcept
{
    set_error(detail::map_awaitable_io_error(io_error, MYSQL_ERROR_RECV));
}

template<RingBufferBackendStrategy Strategy>
bool MysqlStmtExecuteAwaitable<Strategy>::Machine::prepare_read_window()
{
    m_state->read_iov_count = m_state->client->ring_buffer().get_write_iovecs(
        m_state->read_iovecs.data(),
        m_state->read_iovecs.size());
    if (m_state->read_iov_count == 0) {
        set_error(MysqlError(MYSQL_ERROR_RECV, "No writable ring buffer space"));
        return false;
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::MachineAction<typename MysqlStmtExecuteAwaitable<Strategy>::Result>
MysqlStmtExecuteAwaitable<Strategy>::Machine::advance()
{
    if (m_state->result.has_value()) {
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    switch (m_state->phase) {
    case Phase::Invalid:
        set_error(MysqlError(MYSQL_ERROR_INTERNAL, "StmtExecute machine entered invalid state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    case Phase::SendCommand:
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->client->ring_buffer().clear();
            m_state->phase = Phase::ReceivingHeader;
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        return galay::kernel::MachineAction<result_type>::wait_write(
            m_state->encoded_cmd.data() + m_state->sent,
            m_state->encoded_cmd.size() - m_state->sent);
    case Phase::ReceivingHeader:
    case Phase::ReceivingColumns:
    case Phase::ReceivingColumnEof:
    case Phase::ReceivingRows: {
        auto parsed = try_parse_from_ring_buffer();
        if (!parsed.has_value()) {
            set_error(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepare_read_window()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::wait_readv(
            m_state->read_iovecs.data(),
            m_state->read_iov_count);
    }
    case Phase::Done:
        if (!m_state->result.has_value()) {
            m_state->result = std::optional<MysqlResultSet>(std::move(m_state->result_set));
        }
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    set_error(MysqlError(MYSQL_ERROR_INTERNAL, "Unknown stmt-execute machine state"));
    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
}

template<RingBufferBackendStrategy Strategy>
void MysqlStmtExecuteAwaitable<Strategy>::Machine::on_read(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        set_recv_error(result.error());
        return;
    }
    if (result.value() == 0) {
        set_error(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, "Connection closed"));
        return;
    }
    m_state->client->ring_buffer().produce(result.value());
}

template<RingBufferBackendStrategy Strategy>
void MysqlStmtExecuteAwaitable<Strategy>::Machine::on_write(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        set_send_error(result.error());
        return;
    }
    if (result.value() == 0) {
        set_error(MysqlError(MYSQL_ERROR_SEND, "Send returned 0 bytes"));
        return;
    }
    m_state->sent += result.value();
    if (m_state->sent >= m_state->encoded_cmd.size()) {
        m_state->client->ring_buffer().clear();
        m_state->phase = Phase::ReceivingHeader;
    }
}

template<RingBufferBackendStrategy Strategy>
std::expected<bool, MysqlError> MysqlStmtExecuteAwaitable<Strategy>::Machine::try_parse_from_ring_buffer()
{
    while (true) {
        struct iovec read_iovecs[2];
        const size_t read_iovecs_count = m_state->client->ring_buffer().get_read_iovecs(read_iovecs, 2);
        if (read_iovecs_count == 0) {
            return false;
        }

        auto linear = detail::linearize_read_iovecs(
            std::span<const struct iovec>(read_iovecs, read_iovecs_count),
            m_state->parse_scratch);
        const char* data = linear.data();
        size_t len = linear.size();

        size_t consumed = 0;
        auto pkt = m_state->client->parser().extract_packet(data, len, consumed);
        if (!pkt) {
            if (pkt.error() == protocol::ParseError::Incomplete) {
                return false;
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse stmt-execute packet failed"));
        }

        const uint8_t first_byte = static_cast<uint8_t>(pkt->payload[0]);
        const uint32_t caps = m_state->client->server_capabilities();

        if (m_state->phase == Phase::ReceivingHeader) {
            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parse_err(pkt->payload, pkt->payload_len, caps);
                m_state->client->ring_buffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Execute failed"));
            }

            if (first_byte == 0x00) {
                auto ok = m_state->client->parser().parse_ok(pkt->payload, pkt->payload_len, caps);
                m_state->client->ring_buffer().consume(consumed);
                if (!ok) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse OK packet"));
                }
                m_state->result_set.set_affected_rows(ok->affected_rows);
                m_state->result_set.set_last_insert_id(ok->last_insert_id);
                m_state->result_set.set_warnings(ok->warnings);
                m_state->result_set.set_status_flags(ok->status_flags);
                m_state->phase = Phase::Done;
                return true;
            }

            size_t int_consumed = 0;
            auto col_count = protocol::read_len_enc_int(pkt->payload, pkt->payload_len, int_consumed);
            if (!col_count) {
                m_state->client->ring_buffer().consume(consumed);
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse column count"));
            }
            m_state->column_count = col_count.value();
            m_state->columns_received = 0;
            m_state->result_set.reserve_fields(static_cast<size_t>(m_state->column_count));
            m_state->client->ring_buffer().consume(consumed);
            m_state->phase = Phase::ReceivingColumns;
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumns) {
            auto col = m_state->client->parser().parse_column_definition(pkt->payload, pkt->payload_len);
            m_state->client->ring_buffer().consume(consumed);
            if (!col) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse column definition failed"));
            }

            MysqlField field(col->name,
                             static_cast<MysqlFieldType>(col->column_type),
                             col->flags,
                             col->column_length,
                             col->decimals);
            field.set_catalog(col->catalog);
            field.set_schema(col->schema);
            field.set_table(col->table);
            field.set_org_table(col->org_table);
            field.set_org_name(col->org_name);
            field.set_character_set(col->character_set);
            m_state->result_set.add_field(std::move(field));

            ++m_state->columns_received;
            if (m_state->columns_received >= m_state->column_count) {
                m_state->phase = (caps & protocol::CLIENT_DEPRECATE_EOF)
                    ? Phase::ReceivingRows
                    : Phase::ReceivingColumnEof;
            }
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumnEof) {
            m_state->client->ring_buffer().consume(consumed);
            m_state->phase = Phase::ReceivingRows;
            continue;
        }

        if (m_state->phase == Phase::ReceivingRows) {
            if (first_byte == 0xFE && pkt->payload_len < 0xFFFFFF) {
                if (caps & protocol::CLIENT_DEPRECATE_EOF) {
                    auto ok = m_state->client->parser().parse_ok(pkt->payload, pkt->payload_len, caps);
                    if (ok) {
                        m_state->result_set.set_warnings(ok->warnings);
                        m_state->result_set.set_status_flags(ok->status_flags);
                    }
                } else {
                    auto eof = m_state->client->parser().parse_eof(pkt->payload, pkt->payload_len);
                    if (eof) {
                        m_state->result_set.set_warnings(eof->warnings);
                        m_state->result_set.set_status_flags(eof->status_flags);
                    }
                }
                m_state->client->ring_buffer().consume(consumed);
                m_state->phase = Phase::Done;
                return true;
            }

            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parse_err(pkt->payload, pkt->payload_len, caps);
                m_state->client->ring_buffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Error during row fetch"));
            }

            auto row = m_state->client->parser().parse_text_row(
                pkt->payload,
                pkt->payload_len,
                m_state->column_count);
            m_state->client->ring_buffer().consume(consumed);
            if (!row) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse row failed"));
            }
            m_state->result_set.add_row(MysqlRow(std::move(row.value())));
            continue;
        }

        return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Invalid stmt-execute parser state"));
    }
}

// ======================== MysqlPipelineAwaitable<Strategy> ========================

template<RingBufferBackendStrategy Strategy>
MysqlPipelineAwaitable<Strategy>::MysqlPipelineAwaitable(AsyncMysqlClient<Strategy>& client,
                                               std::span<const protocol::MysqlCommandView> commands)
    : m_state(std::make_shared<SharedState>(client, commands))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::from_state_machine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

template<RingBufferBackendStrategy Strategy>
bool MysqlPipelineAwaitable<Strategy>::is_invalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
MysqlPipelineAwaitable<Strategy>::SharedState::SharedState(
    AsyncMysqlClient<Strategy>& client,
    std::span<const protocol::MysqlCommandView> commands)
    : client(&client)
    , expected_results(commands.size())
    , phase(commands.empty() ? Phase::Done : Phase::SendCommands)
{
    results.reserve(expected_results);
    if (client.async_config().result_row_reserve_hint > 0) {
        current_result.reserve_rows(client.async_config().result_row_reserve_hint);
    }

    if (commands.empty()) {
        result = std::optional<std::vector<MysqlResultSet>>(std::vector<MysqlResultSet>{});
        return;
    }

    size_t encoded_bytes = 0;
    for (const auto& cmd : commands) {
        if (cmd.encoded.empty()) {
            result = std::unexpected(
                MysqlError(MYSQL_ERROR_INVALID_PARAM, "Pipeline command encoded payload is empty"));
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

template<RingBufferBackendStrategy Strategy>
MysqlPipelineAwaitable<Strategy>::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

template<RingBufferBackendStrategy Strategy>
void MysqlPipelineAwaitable<Strategy>::Machine::set_error(MysqlError error) noexcept
{
    m_state->result = std::unexpected(std::move(error));
    m_state->phase = Phase::Invalid;
}

template<RingBufferBackendStrategy Strategy>
void MysqlPipelineAwaitable<Strategy>::Machine::set_send_error(const IOError& io_error) noexcept
{
    set_error(detail::map_awaitable_io_error(io_error, MYSQL_ERROR_SEND));
}

template<RingBufferBackendStrategy Strategy>
void MysqlPipelineAwaitable<Strategy>::Machine::set_recv_error(const IOError& io_error) noexcept
{
    set_error(detail::map_awaitable_io_error(io_error, MYSQL_ERROR_RECV));
}

template<RingBufferBackendStrategy Strategy>
void MysqlPipelineAwaitable<Strategy>::Machine::refill_write_iov_window()
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

template<RingBufferBackendStrategy Strategy>
size_t MysqlPipelineAwaitable<Strategy>::Machine::pending_write_iov_count()
{
    while (m_state->write_iov_cursor < m_state->write_iovecs.size() &&
           m_state->write_iovecs[m_state->write_iov_cursor].iov_len == 0) {
        ++m_state->write_iov_cursor;
    }

    if (m_state->write_iov_cursor >= m_state->write_iovecs.size()) {
        refill_write_iov_window();
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

template<RingBufferBackendStrategy Strategy>
bool MysqlPipelineAwaitable<Strategy>::Machine::advance_after_write(size_t sent_bytes)
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
        refill_write_iov_window();
    }

    return true;
}

template<RingBufferBackendStrategy Strategy>
bool MysqlPipelineAwaitable<Strategy>::Machine::prepare_read_window()
{
    m_state->read_iov_count = m_state->client->ring_buffer().get_write_iovecs(
        m_state->read_iovecs.data(),
        m_state->read_iovecs.size());
    if (m_state->read_iov_count == 0) {
        set_error(MysqlError(
            MYSQL_ERROR_RECV,
            "No writable ring buffer space while receiving pipeline response"));
        return false;
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
void MysqlPipelineAwaitable<Strategy>::Machine::reset_current_result()
{
    m_state->phase = Phase::ReceivingHeader;
    m_state->current_result = MysqlResultSet{};
    if (m_state->client->async_config().result_row_reserve_hint > 0) {
        m_state->current_result.reserve_rows(m_state->client->async_config().result_row_reserve_hint);
    }
    m_state->column_count = 0;
    m_state->columns_received = 0;
}

template<RingBufferBackendStrategy Strategy>
void MysqlPipelineAwaitable<Strategy>::Machine::finalize_current_result()
{
    m_state->results.push_back(std::move(m_state->current_result));
    if (m_state->results.size() >= m_state->expected_results) {
        m_state->phase = Phase::Done;
    } else {
        reset_current_result();
    }
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::MachineAction<typename MysqlPipelineAwaitable<Strategy>::Result>
MysqlPipelineAwaitable<Strategy>::Machine::advance()
{
    if (m_state->result.has_value()) {
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    switch (m_state->phase) {
    case Phase::Invalid:
        set_error(MysqlError(MYSQL_ERROR_INTERNAL, "Pipeline machine entered invalid state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    case Phase::SendCommands: {
        const size_t pending = pending_write_iov_count();
        if (pending == 0) {
            m_state->client->ring_buffer().clear();
            m_state->phase = Phase::ReceivingHeader;
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        return galay::kernel::MachineAction<result_type>::wait_writev(
            m_state->write_iovecs.data() + m_state->write_iov_cursor,
            pending);
    }
    case Phase::ReceivingHeader:
    case Phase::ReceivingColumns:
    case Phase::ReceivingColumnEof:
    case Phase::ReceivingRows: {
        auto parsed = try_parse_from_ring_buffer();
        if (!parsed.has_value()) {
            set_error(std::move(parsed.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (parsed.value()) {
            return galay::kernel::MachineAction<result_type>::continue_();
        }
        if (!prepare_read_window()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        return galay::kernel::MachineAction<result_type>::wait_readv(
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

    set_error(MysqlError(MYSQL_ERROR_INTERNAL, "Unknown pipeline machine state"));
    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
}

template<RingBufferBackendStrategy Strategy>
void MysqlPipelineAwaitable<Strategy>::Machine::on_read(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result.has_value()) {
        set_recv_error(result.error());
        return;
    }
    if (result.value() == 0) {
        set_error(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, "Connection closed"));
        return;
    }
    m_state->client->ring_buffer().produce(result.value());
}

template<RingBufferBackendStrategy Strategy>
void MysqlPipelineAwaitable<Strategy>::Machine::on_write(std::expected<size_t, IOError> result)
{
    if (m_state->result.has_value()) {
        return;
    }
    if (!result) {
        set_send_error(result.error());
        return;
    }

    const size_t sent = result.value();
    if (sent == 0) {
        set_error(MysqlError(MYSQL_ERROR_SEND, "Send returned 0 bytes in pipeline"));
        return;
    }

    if (!advance_after_write(sent)) {
        set_send_error(IOError(galay::kernel::kSendFailed, 0));
        return;
    }

    if (pending_write_iov_count() == 0) {
        m_state->client->ring_buffer().clear();
        m_state->phase = Phase::ReceivingHeader;
    }
}
template<RingBufferBackendStrategy Strategy>
std::expected<bool, MysqlError> MysqlPipelineAwaitable<Strategy>::Machine::try_parse_from_ring_buffer()
{
    while (m_state->results.size() < m_state->expected_results) {
        struct iovec read_iovecs[2];
        const size_t read_iovecs_count = m_state->client->ring_buffer().get_read_iovecs(read_iovecs, 2);
        if (read_iovecs_count == 0) {
            return false;
        }

        auto linear = detail::linearize_read_iovecs(
            std::span<const struct iovec>(read_iovecs, read_iovecs_count),
            m_state->parse_scratch);
        const char* data = linear.data();
        size_t len = linear.size();

        size_t consumed = 0;
        auto pkt = m_state->client->parser().extract_packet(data, len, consumed);
        if (!pkt) {
            if (pkt.error() == protocol::ParseError::Incomplete) {
                return false;
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Parse pipeline packet failed"));
        }

        const uint8_t first_byte = static_cast<uint8_t>(pkt->payload[0]);
        const uint32_t caps = m_state->client->server_capabilities();

        if (m_state->phase == Phase::ReceivingHeader) {
            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parse_err(pkt->payload, pkt->payload_len, caps);
                m_state->client->ring_buffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Pipeline query failed"));
            }

            if (first_byte == 0x00) {
                auto ok = m_state->client->parser().parse_ok(pkt->payload, pkt->payload_len, caps);
                m_state->client->ring_buffer().consume(consumed);
                if (!ok) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse OK packet"));
                }

                m_state->current_result.set_affected_rows(ok->affected_rows);
                m_state->current_result.set_last_insert_id(ok->last_insert_id);
                m_state->current_result.set_warnings(ok->warnings);
                m_state->current_result.set_status_flags(ok->status_flags);
                m_state->current_result.set_info(ok->info);
                finalize_current_result();
                continue;
            }

            size_t int_consumed = 0;
            auto col_count = protocol::read_len_enc_int(pkt->payload, pkt->payload_len, int_consumed);
            if (!col_count) {
                m_state->client->ring_buffer().consume(consumed);
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse column count"));
            }

            m_state->column_count = col_count.value();
            m_state->columns_received = 0;
            m_state->current_result.reserve_fields(static_cast<size_t>(m_state->column_count));
            m_state->client->ring_buffer().consume(consumed);
            m_state->phase = Phase::ReceivingColumns;
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumns) {
            auto col = m_state->client->parser().parse_column_definition(pkt->payload, pkt->payload_len);
            m_state->client->ring_buffer().consume(consumed);
            if (!col) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse column definition"));
            }

            MysqlField field(col->name,
                             static_cast<MysqlFieldType>(col->column_type),
                             col->flags,
                             col->column_length,
                             col->decimals);
            field.set_catalog(col->catalog);
            field.set_schema(col->schema);
            field.set_table(col->table);
            field.set_org_table(col->org_table);
            field.set_org_name(col->org_name);
            field.set_character_set(col->character_set);
            m_state->current_result.add_field(std::move(field));

            ++m_state->columns_received;
            if (m_state->columns_received >= m_state->column_count) {
                m_state->phase = (caps & protocol::CLIENT_DEPRECATE_EOF)
                    ? Phase::ReceivingRows
                    : Phase::ReceivingColumnEof;
            }
            continue;
        }

        if (m_state->phase == Phase::ReceivingColumnEof) {
            m_state->client->ring_buffer().consume(consumed);
            m_state->phase = Phase::ReceivingRows;
            continue;
        }

        if (m_state->phase == Phase::ReceivingRows) {
            if (first_byte == 0xFE && pkt->payload_len < 0xFFFFFF) {
                if (caps & protocol::CLIENT_DEPRECATE_EOF) {
                    auto ok = m_state->client->parser().parse_ok(pkt->payload, pkt->payload_len, caps);
                    if (ok) {
                        m_state->current_result.set_warnings(ok->warnings);
                        m_state->current_result.set_status_flags(ok->status_flags);
                    }
                } else {
                    auto eof = m_state->client->parser().parse_eof(pkt->payload, pkt->payload_len);
                    if (eof) {
                        m_state->current_result.set_warnings(eof->warnings);
                        m_state->current_result.set_status_flags(eof->status_flags);
                    }
                }

                m_state->client->ring_buffer().consume(consumed);
                finalize_current_result();
                continue;
            }

            if (first_byte == 0xFF) {
                auto err = m_state->client->parser().parse_err(pkt->payload, pkt->payload_len, caps);
                m_state->client->ring_buffer().consume(consumed);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Pipeline row fetch failed"));
            }

            auto row = m_state->client->parser().parse_text_row(
                pkt->payload,
                pkt->payload_len,
                m_state->column_count);
            m_state->client->ring_buffer().consume(consumed);
            if (!row) {
                return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse text row"));
            }
            m_state->current_result.add_row(MysqlRow(std::move(row.value())));
            continue;
        }

        return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Invalid pipeline parser state"));
    }

    m_state->phase = Phase::Done;
    return true;
}

} // namespace details
} // namespace galay::mysql

#endif // GALAY_MYSQL_DETAILS_AWAITABLE_INL
