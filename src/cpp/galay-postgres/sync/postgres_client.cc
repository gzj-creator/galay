#include "postgres_client.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace galay::postgres
{

namespace
{

std::string_view linearize(std::span<const struct iovec> iovecs, std::string& scratch)
{
    if (iovecs.empty()) {
        return {};
    }
    if (iovecs.size() == 1) {
        return {static_cast<const char*>(iovecs[0].iov_base), iovecs[0].iov_len};
    }
    scratch.clear();
    for (const auto& iovec : iovecs) {
        if (iovec.iov_len != 0) {
            scratch.append(static_cast<const char*>(iovec.iov_base), iovec.iov_len);
        }
    }
    return scratch;
}

PostgresError systemError(PostgresErrorType type, std::string_view prefix, int error_number)
{
    return PostgresError(type,
                         std::string(prefix) + ": " + std::string(std::strerror(error_number)));
}

PostgresError protocolError(std::string_view message)
{
    return PostgresError(POSTGRES_ERROR_PROTOCOL, std::string(message));
}

PostgresError serverError(protocol::ErrorFields fields, PostgresErrorType type)
{
    std::string message = std::move(fields.message);
    if (!fields.detail.empty()) {
        message += "; detail: " + fields.detail;
    }
    if (!fields.hint.empty()) {
        message += "; hint: " + fields.hint;
    }
    return PostgresError(type,
                         std::move(fields.sql_state),
                         std::move(fields.severity),
                         std::move(message));
}

std::expected<protocol::ErrorFields, PostgresError>
parseServerError(const protocol::PostgresParser& parser, std::string_view payload)
{
    auto parsed = parser.parseErrorResponse(payload.data(), payload.size());
    if (!parsed) {
        return std::unexpected(protocolError("Malformed PostgreSQL ErrorResponse"));
    }
    return std::move(*parsed);
}

} // namespace

PostgresClient::PrepareResult PostgresClient::PrepareResult::clone() const
{
    PrepareResult copy;
    copy.statement_name = statement_name;
    copy.parameter_types = parameter_types;
    copy.fields.reserve(fields.size());
    for (const auto& field : fields) {
        copy.fields.push_back(field.clone());
    }
    return copy;
}

PostgresClient::PostgresClient()
    : m_recv_ring_buffer(kRecvBufferCapacity)
{
}

PostgresClient::~PostgresClient()
{
    close();
}

PostgresClient::PostgresClient(PostgresClient&& other) noexcept
    : m_recv_ring_buffer(std::move(other.m_recv_ring_buffer))
    , m_server_parameters(std::move(other.m_server_parameters))
    , m_parse_scratch(std::move(other.m_parse_scratch))
    , m_backend_key_data(std::move(other.m_backend_key_data))
    , m_parser(std::move(other.m_parser))
    , m_encoder(std::move(other.m_encoder))
    , m_socket_fd(std::exchange(other.m_socket_fd, -1))
    , m_transaction_status(other.m_transaction_status)
    , m_connected(std::exchange(other.m_connected, false))
{
    other.m_transaction_status = 'I';
}

PostgresClient& PostgresClient::operator=(PostgresClient&& other) noexcept
{
    if (this != &other) {
        close();
        m_recv_ring_buffer = std::move(other.m_recv_ring_buffer);
        m_server_parameters = std::move(other.m_server_parameters);
        m_parse_scratch = std::move(other.m_parse_scratch);
        m_backend_key_data = std::move(other.m_backend_key_data);
        m_parser = std::move(other.m_parser);
        m_encoder = std::move(other.m_encoder);
        m_socket_fd = std::exchange(other.m_socket_fd, -1);
        m_transaction_status = other.m_transaction_status;
        m_connected = std::exchange(other.m_connected, false);
        other.m_transaction_status = 'I';
    }
    return *this;
}

PostgresVoidResult PostgresClient::connectSocket(const std::string& host,
                                                 uint16_t port,
                                                 uint32_t timeout_ms,
                                                 bool tcp_no_delay)
{
    closeSocket();
    m_socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket_fd < 0) {
        return std::unexpected(systemError(POSTGRES_ERROR_CONNECTION,
                                           "Failed to create PostgreSQL socket",
                                           errno));
    }

    const int original_flags = ::fcntl(m_socket_fd, F_GETFL, 0);
    if (original_flags < 0 || ::fcntl(m_socket_fd, F_SETFL, original_flags | O_NONBLOCK) != 0) {
        const int saved_errno = errno;
        closeSocket();
        return std::unexpected(systemError(POSTGRES_ERROR_CONNECTION,
                                           "Failed to configure non-blocking connect",
                                           saved_errno));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        const int lookup = ::getaddrinfo(host.c_str(), nullptr, &hints, &addresses);
        if (lookup != 0 || addresses == nullptr) {
            if (addresses != nullptr) {
                ::freeaddrinfo(addresses);
            }
            closeSocket();
            return std::unexpected(PostgresError(POSTGRES_ERROR_CONNECTION,
                                                  "Failed to resolve host " + host));
        }
        address.sin_addr = reinterpret_cast<sockaddr_in*>(addresses->ai_addr)->sin_addr;
        ::freeaddrinfo(addresses);
    }

    int connected = ::connect(m_socket_fd,
                              reinterpret_cast<sockaddr*>(&address),
                              sizeof(address));
    if (connected != 0 && errno != EINPROGRESS) {
        const int saved_errno = errno;
        closeSocket();
        return std::unexpected(systemError(POSTGRES_ERROR_CONNECTION,
                                           "PostgreSQL connect failed",
                                           saved_errno));
    }
    if (connected != 0) {
        pollfd descriptor{.fd = m_socket_fd, .events = POLLOUT, .revents = 0};
        int poll_result = 0;
        do {
            poll_result = ::poll(&descriptor,
                                 1,
                                 timeout_ms > static_cast<uint32_t>(std::numeric_limits<int>::max())
                                     ? std::numeric_limits<int>::max()
                                     : static_cast<int>(timeout_ms));
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result == 0) {
            closeSocket();
            return std::unexpected(PostgresError(POSTGRES_ERROR_TIMEOUT,
                                                  "PostgreSQL connection timed out"));
        }
        if (poll_result < 0) {
            const int saved_errno = errno;
            closeSocket();
            return std::unexpected(systemError(POSTGRES_ERROR_CONNECTION,
                                               "Polling PostgreSQL connection failed",
                                               saved_errno));
        }

        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (::getsockopt(m_socket_fd,
                         SOL_SOCKET,
                         SO_ERROR,
                         &socket_error,
                         &error_length) != 0 || socket_error != 0) {
            const int saved_errno = socket_error != 0 ? socket_error : errno;
            closeSocket();
            return std::unexpected(systemError(POSTGRES_ERROR_CONNECTION,
                                               "PostgreSQL connect failed",
                                               saved_errno));
        }
    }

    if (::fcntl(m_socket_fd, F_SETFL, original_flags) != 0) {
        const int saved_errno = errno;
        closeSocket();
        return std::unexpected(systemError(POSTGRES_ERROR_CONNECTION,
                                           "Failed to restore socket flags",
                                           saved_errno));
    }
    if (tcp_no_delay) {
        const int enabled = 1;
        if (::setsockopt(m_socket_fd,
                         IPPROTO_TCP,
                         TCP_NODELAY,
                         &enabled,
                         sizeof(enabled)) != 0) {
            const int saved_errno = errno;
            closeSocket();
            return std::unexpected(systemError(POSTGRES_ERROR_CONNECTION,
                                               "Failed to enable TCP_NODELAY",
                                               saved_errno));
        }
    }

    m_connected = true;
    m_recv_ring_buffer.clear();
    m_parse_scratch.clear();
    return {};
}

void PostgresClient::closeSocket() noexcept
{
    if (m_socket_fd >= 0) {
        // close() has no useful recovery path in this noexcept cleanup boundary.
        const int close_result = ::close(m_socket_fd);
        (void)close_result;
        m_socket_fd = -1;
    }
    m_connected = false;
    m_transaction_status = 'I';
    m_recv_ring_buffer.clear();
    m_parse_scratch.clear();
    m_server_parameters.clear();
    m_backend_key_data.reset();
}

PostgresVoidResult PostgresClient::connect(const PostgresConfig& config)
{
    if (config.host.empty() || config.username.empty() || config.port == 0) {
        return std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                              "Host, username, and port are required"));
    }
    auto socket_result = connectSocket(config.host,
                                       config.port,
                                       config.connect_timeout_ms,
                                       config.tcp_no_delay);
    if (!socket_result) {
        return std::unexpected(socket_result.error());
    }

    const std::string startup = m_encoder.encodeStartupMessage(config);
    if (startup.empty()) {
        closeSocket();
        return std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                              "Invalid PostgreSQL startup parameters"));
    }
    auto sent = sendAll(startup);
    if (!sent) {
        closeSocket();
        return std::unexpected(sent.error());
    }

    protocol::ScramSha256 scram;
    bool scram_started = false;
    bool scram_verified = false;
    bool authentication_ok = false;
    while (true) {
        auto message_result = recvMessage();
        if (!message_result) {
            closeSocket();
            return std::unexpected(message_result.error());
        }
        Message message = std::move(*message_result);
        if (message.type == protocol::kMsgAuthentication) {
            auto auth = m_parser.parseAuthenticationRequest(message.payload.data(),
                                                            message.payload.size());
            if (!auth) {
                closeSocket();
                return std::unexpected(protocolError("Malformed AuthenticationRequest"));
            }
            switch (auth->kind) {
            case protocol::AuthRequestKind::Ok:
                authentication_ok = true;
                break;
            case protocol::AuthRequestKind::Sasl: {
                if (std::find(auth->mechanisms.begin(),
                              auth->mechanisms.end(),
                              "SCRAM-SHA-256") == auth->mechanisms.end()) {
                    closeSocket();
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH,
                                                          "Server does not offer SCRAM-SHA-256"));
                }
                auto nonce = protocol::ScramSha256::generateNonce();
                auto first = nonce ? scram.clientFirstMessage({}, *nonce)
                                   : std::expected<std::string, std::string>(
                                         std::unexpected(nonce.error()));
                if (!first) {
                    closeSocket();
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH, first.error()));
                }
                const std::string response =
                    m_encoder.encodeSASLInitialResponse("SCRAM-SHA-256", *first);
                auto response_result = sendAll(response);
                if (!response_result) {
                    closeSocket();
                    return std::unexpected(response_result.error());
                }
                scram_started = true;
                break;
            }
            case protocol::AuthRequestKind::SaslContinue: {
                if (!scram_started) {
                    closeSocket();
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH,
                                                          "Unexpected SCRAM continuation"));
                }
                auto parsed = scram.parseServerFirst(auth->data);
                auto final_message = parsed ? scram.clientFinalMessage(config.password)
                                            : std::expected<std::string, std::string>(
                                                  std::unexpected(parsed.error()));
                if (!final_message) {
                    closeSocket();
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH,
                                                          final_message.error()));
                }
                auto response_result = sendAll(m_encoder.encodeSASLResponse(*final_message));
                if (!response_result) {
                    closeSocket();
                    return std::unexpected(response_result.error());
                }
                break;
            }
            case protocol::AuthRequestKind::SaslFinal: {
                auto verified = scram.verifyServerFinal(auth->data);
                if (!verified) {
                    closeSocket();
                    return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH,
                                                          verified.error()));
                }
                scram_verified = true;
                break;
            }
            case protocol::AuthRequestKind::Md5Password: {
                if (auth->data.size() != 4) {
                    closeSocket();
                    return std::unexpected(protocolError("Malformed MD5 authentication salt"));
                }
                std::array<uint8_t, 4> salt{};
                std::copy_n(reinterpret_cast<const uint8_t*>(auth->data.data()), 4, salt.begin());
                auto response_result = sendAll(m_encoder.encodePasswordMessage(
                    protocol::md5Password(config.username, config.password, salt)));
                if (!response_result) {
                    closeSocket();
                    return std::unexpected(response_result.error());
                }
                break;
            }
            case protocol::AuthRequestKind::CleartextPassword: {
                auto response_result = sendAll(m_encoder.encodePasswordMessage(config.password));
                if (!response_result) {
                    closeSocket();
                    return std::unexpected(response_result.error());
                }
                break;
            }
            default:
                closeSocket();
                return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH,
                                                      "Unsupported PostgreSQL authentication method"));
            }
            continue;
        }
        if (message.type == protocol::kMsgParameterStatus) {
            auto status = m_parser.parseParameterStatus(message.payload.data(), message.payload.size());
            if (!status) {
                closeSocket();
                return std::unexpected(protocolError("Malformed ParameterStatus"));
            }
            m_server_parameters.insert_or_assign(std::move(status->name), std::move(status->value));
            continue;
        }
        if (message.type == protocol::kMsgBackendKeyData) {
            auto key = m_parser.parseBackendKeyData(message.payload.data(), message.payload.size());
            if (!key) {
                closeSocket();
                return std::unexpected(protocolError("Malformed BackendKeyData"));
            }
            m_backend_key_data = *key;
            continue;
        }
        if (message.type == protocol::kMsgNoticeResponse) {
            continue;
        }
        if (message.type == protocol::kMsgErrorResponse) {
            auto fields = parseServerError(m_parser, message.payload);
            closeSocket();
            return fields
                ? std::unexpected(serverError(std::move(*fields), POSTGRES_ERROR_AUTH))
                : std::unexpected(fields.error());
        }
        if (message.type == protocol::kMsgReadyForQuery) {
            auto ready = m_parser.parseReadyForQuery(message.payload.data(), message.payload.size());
            if (!ready || !authentication_ok || (scram_started && !scram_verified)) {
                closeSocket();
                return std::unexpected(PostgresError(POSTGRES_ERROR_AUTH,
                                                      "Authentication did not complete before ReadyForQuery"));
            }
            m_transaction_status = ready->transaction_status;
            return {};
        }
        closeSocket();
        return std::unexpected(protocolError("Unexpected message during PostgreSQL startup"));
    }
}

PostgresVoidResult PostgresClient::connect(const std::string& host,
                                           uint16_t port,
                                           const std::string& user,
                                           const std::string& password,
                                           const std::string& database)
{
    return connect(PostgresConfig::create(host, port, user, password, database));
}

PostgresVoidResult PostgresClient::sendAll(std::string_view data)
{
    if (!m_connected) {
        return std::unexpected(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                                              "PostgreSQL connection is closed"));
    }
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = ::send(m_socket_fd,
                                     data.data() + sent,
                                     data.size() - sent,
                                     MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            const int saved_errno = errno;
            m_connected = false;
            return std::unexpected(systemError(POSTGRES_ERROR_SEND,
                                               "PostgreSQL send failed",
                                               saved_errno));
        }
        if (count == 0) {
            m_connected = false;
            return std::unexpected(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                                                  "Connection closed during send"));
        }
        sent += static_cast<size_t>(count);
    }
    return {};
}

PostgresVoidResult PostgresClient::sendAllv(std::span<const struct iovec> iovecs)
{
    if (!m_connected) {
        return std::unexpected(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                                              "PostgreSQL connection is closed"));
    }
    size_t index = 0;
    size_t offset = 0;
    while (index < iovecs.size()) {
        std::vector<struct iovec> window;
        window.reserve(std::min<size_t>(iovecs.size() - index, 1024));
        for (size_t cursor = index; cursor < iovecs.size() && window.size() < 1024; ++cursor) {
            if (iovecs[cursor].iov_len == 0 || (cursor == index && offset >= iovecs[cursor].iov_len)) {
                continue;
            }
            struct iovec item = iovecs[cursor];
            if (cursor == index && offset != 0) {
                item.iov_base = static_cast<char*>(item.iov_base) + offset;
                item.iov_len -= offset;
            }
            window.push_back(item);
        }
        if (window.empty()) {
            ++index;
            offset = 0;
            continue;
        }
        const ssize_t count = ::writev(m_socket_fd,
                                       window.data(),
                                       static_cast<int>(window.size()));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            const int saved_errno = count < 0 ? errno : ECONNRESET;
            m_connected = false;
            return std::unexpected(systemError(POSTGRES_ERROR_SEND,
                                               "PostgreSQL writev failed",
                                               saved_errno));
        }
        size_t consumed = static_cast<size_t>(count);
        while (consumed != 0 && index < iovecs.size()) {
            if (iovecs[index].iov_len <= offset) {
                ++index;
                offset = 0;
                continue;
            }
            const size_t remaining = iovecs[index].iov_len - offset;
            if (consumed < remaining) {
                offset += consumed;
                consumed = 0;
            } else {
                consumed -= remaining;
                ++index;
                offset = 0;
            }
        }
    }
    return {};
}

PostgresVoidResult PostgresClient::recvIntoRingBuffer()
{
    if (!m_connected) {
        return std::unexpected(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                                              "PostgreSQL connection is closed"));
    }
    std::array<struct iovec, 2> iovecs{};
    const size_t count = m_recv_ring_buffer.getWriteIovecs(iovecs);
    if (count == 0) {
        return std::unexpected(PostgresError(POSTGRES_ERROR_BUFFER_OVERFLOW,
                                              "PostgreSQL response exceeds receive buffer capacity"));
    }
    ssize_t received = 0;
    do {
        received = ::readv(m_socket_fd, iovecs.data(), static_cast<int>(count));
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        const int saved_errno = errno;
        m_connected = false;
        return std::unexpected(systemError(POSTGRES_ERROR_RECV,
                                           "PostgreSQL readv failed",
                                           saved_errno));
    }
    if (received == 0) {
        m_connected = false;
        return std::unexpected(PostgresError(POSTGRES_ERROR_CONNECTION_CLOSED,
                                              "Connection closed during receive"));
    }
    m_recv_ring_buffer.produce(static_cast<size_t>(received));
    return {};
}

std::expected<std::optional<PostgresClient::Message>, PostgresError>
PostgresClient::tryExtractMessage()
{
    std::array<struct iovec, 2> iovecs{};
    const size_t count = m_recv_ring_buffer.getReadIovecs(iovecs);
    if (count == 0) {
        return std::optional<Message>{};
    }
    const std::string_view bytes = linearize(std::span<const struct iovec>(iovecs.data(), count),
                                             m_parse_scratch);
    auto view = m_parser.extractMessage(bytes.data(), bytes.size());
    if (!view) {
        if (view.error() == protocol::ParseError::Incomplete) {
            return std::optional<Message>{};
        }
        return std::unexpected(protocolError("Malformed PostgreSQL message frame"));
    }
    Message message{.type = view->type,
                    .payload = std::string(view->payload, view->payload_len)};
    m_recv_ring_buffer.consume(view->consumed);
    return std::optional<Message>(std::move(message));
}

std::expected<PostgresClient::Message, PostgresError> PostgresClient::recvMessage()
{
    while (true) {
        auto parsed = tryExtractMessage();
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (parsed->has_value()) {
            return std::move(parsed->value());
        }
        auto received = recvIntoRingBuffer();
        if (!received) {
            return std::unexpected(received.error());
        }
    }
}

PostgresResult PostgresClient::receiveResultUntilReady()
{
    PostgresResultSet result;
    std::optional<PostgresError> pending_error;
    while (true) {
        auto received = recvMessage();
        if (!received) {
            return std::unexpected(received.error());
        }
        Message message = std::move(*received);
        switch (message.type) {
        case protocol::kMsgRowDescription: {
            auto fields = m_parser.parseRowDescription(message.payload.data(), message.payload.size());
            if (!fields) {
                return std::unexpected(protocolError("Malformed RowDescription"));
            }
            result.reserveFields(fields->size());
            for (auto& field : *fields) {
                result.addField(PostgresField(std::move(field.name),
                                              field.table_oid,
                                              field.column_index,
                                              field.type_oid,
                                              field.type_size,
                                              field.type_modifier,
                                              field.format));
            }
            break;
        }
        case protocol::kMsgDataRow: {
            auto row = m_parser.parseDataRow(message.payload.data(), message.payload.size());
            if (!row || (!result.fields().empty() && row->size() != result.fieldCount())) {
                return std::unexpected(protocolError("Malformed DataRow"));
            }
            result.addRow(std::move(*row));
            break;
        }
        case protocol::kMsgCommandComplete: {
            auto complete = m_parser.parseCommandComplete(message.payload.data(),
                                                          message.payload.size());
            if (!complete) {
                return std::unexpected(protocolError("Malformed CommandComplete"));
            }
            result.setCommandTag(std::move(complete->tag));
            result.setAffectedRows(complete->affected_rows);
            break;
        }
        case protocol::kMsgEmptyQueryResponse:
        case protocol::kMsgParseComplete:
        case protocol::kMsgBindComplete:
        case protocol::kMsgNoData:
        case protocol::kMsgPortalSuspended:
        case protocol::kMsgCloseComplete:
        case protocol::kMsgNoticeResponse:
            break;
        case protocol::kMsgParameterStatus: {
            auto status = m_parser.parseParameterStatus(message.payload.data(), message.payload.size());
            if (!status) {
                return std::unexpected(protocolError("Malformed ParameterStatus"));
            }
            m_server_parameters.insert_or_assign(std::move(status->name), std::move(status->value));
            break;
        }
        case protocol::kMsgErrorResponse: {
            auto fields = parseServerError(m_parser, message.payload);
            if (!fields) {
                return std::unexpected(fields.error());
            }
            if (!pending_error) {
                pending_error = serverError(std::move(*fields), POSTGRES_ERROR_SERVER);
            }
            break;
        }
        case protocol::kMsgReadyForQuery: {
            auto ready = m_parser.parseReadyForQuery(message.payload.data(), message.payload.size());
            if (!ready) {
                return std::unexpected(protocolError("Malformed ReadyForQuery"));
            }
            m_transaction_status = ready->transaction_status;
            if (pending_error) {
                return std::unexpected(std::move(*pending_error));
            }
            return result;
        }
        default:
            return std::unexpected(protocolError("Unexpected PostgreSQL result message"));
        }
    }
}

PostgresResult PostgresClient::query(std::string_view sql)
{
    const std::string command = m_encoder.encodeQuery(sql);
    if (command.empty()) {
        return std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                              "Invalid PostgreSQL query"));
    }
    auto sent = sendAll(command);
    if (!sent) {
        return std::unexpected(sent.error());
    }
    return receiveResultUntilReady();
}

PostgresBatchResult PostgresClient::batch(std::span<const protocol::PostgresCommandView> commands)
{
    if (commands.empty()) {
        return std::vector<PostgresResultSet>{};
    }
    std::vector<struct iovec> iovecs;
    iovecs.reserve(commands.size());
    size_t expected_ready = 0;
    for (const auto& command : commands) {
        if (command.encoded.empty()) {
            return std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                                  "Batch contains an empty command"));
        }
        if (command.kind == protocol::PostgresCommandKind::Query ||
            command.kind == protocol::PostgresCommandKind::Sync) {
            ++expected_ready;
        }
        iovecs.push_back({.iov_base = const_cast<char*>(command.encoded.data()),
                          .iov_len = command.encoded.size()});
    }
    if (expected_ready == 0) {
        return std::unexpected(PostgresError(
            POSTGRES_ERROR_INVALID_PARAM,
            "Batch must contain a Query or Sync ReadyForQuery boundary"));
    }
    auto sent = sendAllv(iovecs);
    if (!sent) {
        return std::unexpected(sent.error());
    }

    std::vector<PostgresResultSet> results;
    results.reserve(expected_ready);
    std::optional<PostgresError> first_error;
    for (size_t index = 0; index < expected_ready; ++index) {
        auto result = receiveResultUntilReady();
        if (result) {
            results.push_back(std::move(*result));
        } else if (!first_error) {
            first_error = result.error();
        }
    }
    if (first_error) {
        return std::unexpected(std::move(*first_error));
    }
    return results;
}

PostgresBatchResult PostgresClient::pipeline(std::span<const std::string_view> sqls)
{
    protocol::PostgresCommandBuilder builder;
    builder.reserve(sqls.size(), 0);
    for (std::string_view sql : sqls) {
        builder.appendQuery(sql);
    }
    return batch(builder.commands());
}

std::expected<PostgresClient::PrepareResult, PostgresError>
PostgresClient::prepare(std::string_view name,
                        std::string_view sql,
                        std::span<const uint32_t> parameter_types)
{
    std::string parse = m_encoder.encodeParse(name, sql, parameter_types);
    std::string describe = m_encoder.encodeDescribeStatement(name);
    std::string sync = m_encoder.encodeSync();
    if (parse.empty() || describe.empty() || sync.empty()) {
        return std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                              "Invalid prepared statement"));
    }
    parse += describe;
    parse += sync;
    std::string command = std::move(parse);
    auto sent = sendAll(command);
    if (!sent) {
        return std::unexpected(sent.error());
    }

    PrepareResult result;
    result.statement_name.assign(name);
    std::optional<PostgresError> pending_error;
    while (true) {
        auto received = recvMessage();
        if (!received) {
            return std::unexpected(received.error());
        }
        Message message = std::move(*received);
        switch (message.type) {
        case protocol::kMsgParseComplete:
        case protocol::kMsgNoData:
        case protocol::kMsgNoticeResponse:
            break;
        case protocol::kMsgParameterDescription: {
            auto parameters = m_parser.parseParameterDescription(message.payload.data(),
                                                                  message.payload.size());
            if (!parameters) {
                return std::unexpected(protocolError("Malformed ParameterDescription"));
            }
            result.parameter_types = std::move(*parameters);
            break;
        }
        case protocol::kMsgRowDescription: {
            auto fields = m_parser.parseRowDescription(message.payload.data(), message.payload.size());
            if (!fields) {
                return std::unexpected(protocolError("Malformed prepared RowDescription"));
            }
            result.fields.reserve(fields->size());
            for (auto& field : *fields) {
                result.fields.emplace_back(std::move(field.name),
                                           field.table_oid,
                                           field.column_index,
                                           field.type_oid,
                                           field.type_size,
                                           field.type_modifier,
                                           field.format);
            }
            break;
        }
        case protocol::kMsgErrorResponse: {
            auto fields = parseServerError(m_parser, message.payload);
            if (!fields) {
                return std::unexpected(fields.error());
            }
            if (!pending_error) {
                pending_error = serverError(std::move(*fields), POSTGRES_ERROR_PREPARED_STMT);
            }
            break;
        }
        case protocol::kMsgReadyForQuery: {
            auto ready = m_parser.parseReadyForQuery(message.payload.data(), message.payload.size());
            if (!ready) {
                return std::unexpected(protocolError("Malformed ReadyForQuery"));
            }
            m_transaction_status = ready->transaction_status;
            if (pending_error) {
                return std::unexpected(std::move(*pending_error));
            }
            return result;
        }
        default:
            return std::unexpected(protocolError("Unexpected prepared statement response"));
        }
    }
}

PostgresResult PostgresClient::execute(
    std::string_view name,
    const std::vector<std::optional<std::string>>& params)
{
    std::string bind = m_encoder.encodeBind({}, name, params);
    std::string describe = m_encoder.encodeDescribePortal({});
    std::string execute = m_encoder.encodeExecute({});
    std::string sync = m_encoder.encodeSync();
    if (bind.empty() || describe.empty() || execute.empty() || sync.empty()) {
        return std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                              "Invalid prepared statement execution"));
    }
    bind += describe;
    bind += execute;
    bind += sync;
    std::string command = std::move(bind);
    auto sent = sendAll(command);
    if (!sent) {
        return std::unexpected(sent.error());
    }
    return receiveResultUntilReady();
}

PostgresVoidResult PostgresClient::closePrepared(std::string_view name)
{
    std::string close = m_encoder.encodeCloseStatement(name);
    std::string sync = m_encoder.encodeSync();
    if (close.empty() || sync.empty()) {
        return std::unexpected(PostgresError(POSTGRES_ERROR_INVALID_PARAM,
                                              "Invalid prepared statement name"));
    }
    close += sync;
    std::string command = std::move(close);
    auto sent = sendAll(command);
    if (!sent) {
        return std::unexpected(sent.error());
    }
    auto result = receiveResultUntilReady();
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

PostgresVoidResult PostgresClient::runSimpleStatement(std::string_view sql)
{
    auto result = query(sql);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

PostgresVoidResult PostgresClient::beginTransaction() { return runSimpleStatement("BEGIN"); }
PostgresVoidResult PostgresClient::commit() { return runSimpleStatement("COMMIT"); }
PostgresVoidResult PostgresClient::rollback() { return runSimpleStatement("ROLLBACK"); }
PostgresVoidResult PostgresClient::ping() { return runSimpleStatement("SELECT 1"); }

void PostgresClient::close() noexcept
{
    if (m_connected) {
        const std::string terminate = m_encoder.encodeTerminate();
        // Terminate is best-effort because the public cleanup boundary is noexcept.
        const auto ignored = sendAll(terminate);
        (void)ignored;
    }
    closeSocket();
}

} // namespace galay::postgres
