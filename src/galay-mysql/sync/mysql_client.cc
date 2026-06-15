#include "mysql_client.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace galay::mysql
{

namespace
{

inline std::string_view linearizeReadIovecs(std::span<const struct iovec> iovecs, std::string& scratch)
{
    if (iovecs.empty()) {
        return {};
    }
    if (iovecs.size() == 1) {
        return std::string_view(static_cast<const char*>(iovecs[0].iov_base), iovecs[0].iov_len);
    }

    scratch.clear();
    for (const auto& iov : iovecs) {
        if (iov.iov_len == 0) continue;
        scratch.append(static_cast<const char*>(iov.iov_base), iov.iov_len);
    }
    return std::string_view(scratch);
}

inline MysqlError makeSysError(MysqlErrorType type, const std::string& prefix)
{
    return MysqlError(type, prefix + ": " + std::string(strerror(errno)));
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

} // namespace

MysqlClient::MysqlClient()
    : m_socket_fd(-1)
    , m_connected(false)
    , m_recv_ring_buffer(kRecvBufferCapacity)
{
}

MysqlClient::~MysqlClient()
{
    close();
}

MysqlClient::MysqlClient(MysqlClient&& other) noexcept
    : m_socket_fd(other.m_socket_fd)
    , m_connected(other.m_connected)
    , m_recv_ring_buffer(std::move(other.m_recv_ring_buffer))
    , m_parse_scratch(std::move(other.m_parse_scratch))
    , m_parser(std::move(other.m_parser))
    , m_encoder(std::move(other.m_encoder))
    , m_server_capabilities(other.m_server_capabilities)
{
    other.m_socket_fd = -1;
    other.m_connected = false;
    other.m_server_capabilities = 0;
}

MysqlClient& MysqlClient::operator=(MysqlClient&& other) noexcept
{
    if (this != &other) {
        close();

        m_socket_fd = other.m_socket_fd;
        m_connected = other.m_connected;
        m_recv_ring_buffer = std::move(other.m_recv_ring_buffer);
        m_parse_scratch = std::move(other.m_parse_scratch);
        m_parser = std::move(other.m_parser);
        m_encoder = std::move(other.m_encoder);
        m_server_capabilities = other.m_server_capabilities;

        other.m_socket_fd = -1;
        other.m_connected = false;
        other.m_server_capabilities = 0;
    }
    return *this;
}

MysqlVoidResult MysqlClient::connectSocket(const std::string& host, uint16_t port, uint32_t timeout_ms)
{
    closeSocket();

    m_socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket_fd < 0) {
        return std::unexpected(makeSysError(MYSQL_ERROR_CONNECTION, "Failed to create socket"));
    }

    const int original_flags = fcntl(m_socket_fd, F_GETFL, 0);
    if (original_flags < 0) {
        closeSocket();
        return std::unexpected(makeSysError(MYSQL_ERROR_CONNECTION, "Failed to get socket flags"));
    }

    if (fcntl(m_socket_fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        closeSocket();
        return std::unexpected(makeSysError(MYSQL_ERROR_CONNECTION, "Failed to set non-block socket"));
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* result = nullptr;
        const int ret = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
        if (ret != 0 || result == nullptr) {
            closeSocket();
            return std::unexpected(MysqlError(MYSQL_ERROR_CONNECTION, "Failed to resolve host: " + host));
        }

        addr.sin_addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr)->sin_addr;
        ::freeaddrinfo(result);
    }

    int ret = ::connect(m_socket_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        closeSocket();
        return std::unexpected(makeSysError(MYSQL_ERROR_CONNECTION, "Connect failed"));
    }

    if (ret < 0) {
        struct pollfd pfd{};
        pfd.fd = m_socket_fd;
        pfd.events = POLLOUT;
        const int poll_ret = ::poll(&pfd, 1, static_cast<int>(timeout_ms));
        if (poll_ret <= 0) {
            closeSocket();
            return std::unexpected(MysqlError(MYSQL_ERROR_TIMEOUT, "Connection timed out"));
        }

        int sock_error = 0;
        socklen_t sock_error_len = sizeof(sock_error);
        if (::getsockopt(m_socket_fd, SOL_SOCKET, SO_ERROR, &sock_error, &sock_error_len) < 0) {
            closeSocket();
            return std::unexpected(makeSysError(MYSQL_ERROR_CONNECTION, "getsockopt failed"));
        }
        if (sock_error != 0) {
            closeSocket();
            return std::unexpected(MysqlError(MYSQL_ERROR_CONNECTION,
                                              "Connect failed: " + std::string(strerror(sock_error))));
        }
    }

    if (fcntl(m_socket_fd, F_SETFL, original_flags) < 0) {
        closeSocket();
        return std::unexpected(makeSysError(MYSQL_ERROR_CONNECTION, "Failed to restore socket flags"));
    }

    const int nodelay = 1;
    (void)::setsockopt(m_socket_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    m_connected = true;
    m_recv_ring_buffer.clear();
    m_parse_scratch.clear();
    return {};
}

void MysqlClient::closeSocket() noexcept
{
    if (m_socket_fd >= 0) {
        ::close(m_socket_fd);
        m_socket_fd = -1;
    }
    m_connected = false;
    m_recv_ring_buffer.clear();
    m_parse_scratch.clear();
}

MysqlVoidResult MysqlClient::connect(const MysqlConfig& config)
{
    auto conn_result = connectSocket(config.host, config.port, config.connect_timeout_ms);
    if (!conn_result) {
        return std::unexpected(conn_result.error());
    }

    auto pkt_result = recvPacket();
    if (!pkt_result) {
        return std::unexpected(pkt_result.error());
    }
    auto& [seq_id, payload] = pkt_result.value();

    if (payload.empty()) {
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Empty handshake payload"));
    }

    if (static_cast<uint8_t>(payload[0]) == 0xFF) {
        auto err = m_parser.parseErr(payload.data(), payload.size(), protocol::CLIENT_PROTOCOL_41);
        if (err) {
            return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
        }
        return std::unexpected(MysqlError(MYSQL_ERROR_CONNECTION, "Server sent error during handshake"));
    }

    auto hs = m_parser.parseHandshake(payload.data(), payload.size());
    if (!hs) {
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse handshake"));
    }
    m_server_capabilities = hs->capability_flags;

    protocol::HandshakeResponse41 resp;
    resp.capability_flags = protocol::CLIENT_PROTOCOL_41
        | protocol::CLIENT_SECURE_CONNECTION
        | protocol::CLIENT_PLUGIN_AUTH
        | protocol::CLIENT_TRANSACTIONS
        | protocol::CLIENT_MULTI_STATEMENTS
        | protocol::CLIENT_MULTI_RESULTS
        | protocol::CLIENT_PS_MULTI_RESULTS
        | protocol::CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA;

    if (!config.database.empty()) {
        resp.capability_flags |= protocol::CLIENT_CONNECT_WITH_DB;
    }

    resp.capability_flags &= hs->capability_flags;
    m_server_capabilities = resp.capability_flags;
    resp.character_set = protocol::CHARSET_UTF8MB4_GENERAL_CI;
    resp.username = config.username;
    resp.database = config.database;
    resp.auth_plugin_name = hs->auth_plugin_name;

    if (hs->auth_plugin_name == "mysql_native_password") {
        resp.auth_response = protocol::AuthPlugin::nativePasswordAuth(config.password, hs->auth_plugin_data);
    } else if (hs->auth_plugin_name == "caching_sha2_password") {
        resp.auth_response = protocol::AuthPlugin::cachingSha2Auth(config.password, hs->auth_plugin_data);
    } else {
        resp.auth_response = protocol::AuthPlugin::nativePasswordAuth(config.password, hs->auth_plugin_data);
        resp.auth_plugin_name = "mysql_native_password";
    }

    auto auth_packet = m_encoder.encodeHandshakeResponse(resp, static_cast<uint8_t>(seq_id + 1));
    auto send_result = sendAll(auth_packet);
    if (!send_result) {
        return std::unexpected(send_result.error());
    }

    auto auth_result = recvPacket();
    if (!auth_result) {
        return std::unexpected(auth_result.error());
    }
    auto& [auth_seq, auth_payload] = auth_result.value();
    (void)auth_seq;

    if (auth_payload.empty()) {
        return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Empty auth response"));
    }

    const uint8_t first_byte = static_cast<uint8_t>(auth_payload[0]);
    if (first_byte == 0x00) {
        return {};
    }

    if (first_byte == 0xFF) {
        auto err = m_parser.parseErr(auth_payload.data(), auth_payload.size(), m_server_capabilities);
        if (err) {
            return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, err->error_code, err->error_message));
        }
        return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Authentication failed"));
    }

    if (first_byte == 0x01) {
        if (auth_payload.size() == 2 && static_cast<uint8_t>(auth_payload[1]) == 0x03) {
            auto ok_result = recvPacket();
            if (!ok_result) {
                return std::unexpected(ok_result.error());
            }
            auto& [ok_seq, ok_payload] = ok_result.value();
            (void)ok_seq;
            if (!ok_payload.empty() && static_cast<uint8_t>(ok_payload[0]) == 0x00) {
                return {};
            }
            if (!ok_payload.empty() && static_cast<uint8_t>(ok_payload[0]) == 0xFF) {
                auto err = m_parser.parseErr(ok_payload.data(), ok_payload.size(), m_server_capabilities);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, err->error_code, err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Authentication failed"));
            }
            return {};
        }

        if (auth_payload.size() == 2 &&
            static_cast<uint8_t>(auth_payload[1]) == 0x04 &&
            hs->auth_plugin_name == "caching_sha2_password") {
            const std::string public_key_request(1, '\x02');
            auto request_packet = encodeRawPacket(public_key_request, static_cast<uint8_t>(auth_seq + 1));
            auto request_result = sendAll(request_packet);
            if (!request_result) {
                return std::unexpected(request_result.error());
            }

            auto public_key_result = recvPacket();
            if (!public_key_result) {
                return std::unexpected(public_key_result.error());
            }
            auto& [public_key_seq, public_key_payload] = public_key_result.value();

            if (public_key_payload.empty()) {
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Empty public key response"));
            }
            if (static_cast<uint8_t>(public_key_payload[0]) == 0xFF) {
                auto err = m_parser.parseErr(public_key_payload.data(),
                                             public_key_payload.size(),
                                             m_server_capabilities);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_AUTH,
                                                      err->error_code,
                                                      err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH,
                                                  "Public key request failed"));
            }

            std::string_view public_key_data = public_key_payload;
            if (!public_key_data.empty() &&
                static_cast<uint8_t>(public_key_data.front()) == 0x01) {
                public_key_data.remove_prefix(1);
            }

            auto encrypted = protocol::AuthPlugin::cachingSha2FullAuth(config.password,
                                                                       hs->auth_plugin_data,
                                                                       public_key_data);
            if (!encrypted) {
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, encrypted.error()));
            }

            auto encrypted_packet = encodeRawPacket(*encrypted,
                                                    static_cast<uint8_t>(public_key_seq + 1));
            auto encrypted_result = sendAll(encrypted_packet);
            if (!encrypted_result) {
                return std::unexpected(encrypted_result.error());
            }

            auto final_result = recvPacket();
            if (!final_result) {
                return std::unexpected(final_result.error());
            }

            auto& [final_seq, final_payload] = final_result.value();
            (void)final_seq;

            if (!final_payload.empty() && static_cast<uint8_t>(final_payload[0]) == 0x00) {
                return {};
            }
            if (!final_payload.empty() && static_cast<uint8_t>(final_payload[0]) == 0xFF) {
                auto err = m_parser.parseErr(final_payload.data(),
                                             final_payload.size(),
                                             m_server_capabilities);
                if (err) {
                    return std::unexpected(MysqlError(MYSQL_ERROR_AUTH,
                                                      err->error_code,
                                                      err->error_message));
                }
                return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Authentication failed"));
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Unexpected full auth response"));
        }

        return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Full auth not supported"));
    }

    return std::unexpected(MysqlError(MYSQL_ERROR_AUTH, "Unexpected auth response"));
}

MysqlVoidResult MysqlClient::connect(const std::string& host, uint16_t port,
                                     const std::string& user, const std::string& password,
                                     const std::string& database)
{
    return connect(MysqlConfig::create(host, port, user, password, database));
}

MysqlVoidResult MysqlClient::sendAll(std::string_view data)
{
    if (!m_connected) {
        return std::unexpected(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, "Not connected"));
    }

    size_t total_sent = 0;
    while (total_sent < data.size()) {
        const ssize_t n = ::send(m_socket_fd,
                                 data.data() + total_sent,
                                 data.size() - total_sent,
                                 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            m_connected = false;
            return std::unexpected(makeSysError(MYSQL_ERROR_SEND, "Send failed"));
        }
        if (n == 0) {
            m_connected = false;
            return std::unexpected(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED,
                                              "Connection closed during send"));
        }
        total_sent += static_cast<size_t>(n);
    }

    return {};
}

MysqlVoidResult MysqlClient::sendAllv(std::span<const struct iovec> iovecs)
{
    if (!m_connected) {
        return std::unexpected(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, "Not connected"));
    }
    if (iovecs.empty()) {
        return {};
    }

    size_t iov_index = 0;
    size_t iov_offset = 0;

#ifdef IOV_MAX
    static constexpr int kMaxWritevIov = IOV_MAX > 0 ? IOV_MAX : 1024;
#else
    static constexpr int kMaxWritevIov = 1024;
#endif

    while (iov_index < iovecs.size()) {
        struct iovec window[kMaxWritevIov];
        int window_count = 0;

        size_t cursor = iov_index;
        size_t cursor_offset = iov_offset;
        while (cursor < iovecs.size() && window_count < kMaxWritevIov) {
            const auto& src = iovecs[cursor];
            if (src.iov_len == 0) {
                ++cursor;
                cursor_offset = 0;
                continue;
            }

            if (cursor_offset >= src.iov_len) {
                ++cursor;
                cursor_offset = 0;
                continue;
            }

            struct iovec out{};
            out.iov_base = static_cast<char*>(src.iov_base) + cursor_offset;
            out.iov_len = src.iov_len - cursor_offset;
            window[window_count++] = out;
            ++cursor;
            cursor_offset = 0;
        }

        if (window_count == 0) {
            break;
        }

        const ssize_t n = ::writev(m_socket_fd, window, window_count);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            m_connected = false;
            return std::unexpected(makeSysError(MYSQL_ERROR_SEND, "Writev failed"));
        }
        if (n == 0) {
            m_connected = false;
            return std::unexpected(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED,
                                              "Connection closed during writev"));
        }

        size_t sent = static_cast<size_t>(n);
        while (sent > 0 && iov_index < iovecs.size()) {
            const size_t cur_len = iovecs[iov_index].iov_len;
            if (cur_len == 0) {
                ++iov_index;
                iov_offset = 0;
                continue;
            }

            const size_t remaining = cur_len - iov_offset;
            if (sent < remaining) {
                iov_offset += sent;
                sent = 0;
            } else {
                sent -= remaining;
                ++iov_index;
                iov_offset = 0;
            }
        }
    }

    return {};
}

MysqlVoidResult MysqlClient::recvIntoRingBuffer()
{
    if (!m_connected) {
        return std::unexpected(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED, "Not connected"));
    }

    struct iovec write_iovecs[2];
    const size_t write_count = m_recv_ring_buffer.getWriteIovecs(write_iovecs, 2);
    if (write_count == 0) {
        return std::unexpected(MysqlError(MYSQL_ERROR_RECV,
                                          "Ring buffer exhausted before parsing complete MySQL responses"));
    }

    ssize_t n = -1;
    while (true) {
        n = ::readv(m_socket_fd, write_iovecs, static_cast<int>(write_count));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    if (n < 0) {
        m_connected = false;
        return std::unexpected(makeSysError(MYSQL_ERROR_RECV, "Readv failed"));
    }

    if (n == 0) {
        m_connected = false;
        return std::unexpected(MysqlError(MYSQL_ERROR_CONNECTION_CLOSED,
                                          "Connection closed during recv"));
    }

    m_recv_ring_buffer.produce(static_cast<size_t>(n));
    return {};
}

std::expected<std::optional<MysqlClient::Packet>, MysqlError> MysqlClient::tryExtractPacket()
{
    struct iovec read_iovecs[2];
    const size_t read_count = m_recv_ring_buffer.getReadIovecs(read_iovecs, 2);
    if (read_count == 0) {
        return std::optional<Packet>{};
    }

    auto linear = linearizeReadIovecs(std::span<const struct iovec>(read_iovecs, read_count), m_parse_scratch);
    if (linear.empty()) {
        return std::optional<Packet>{};
    }

    size_t consumed = 0;
    auto packet = m_parser.extractPacket(linear.data(), linear.size(), consumed);
    if (!packet) {
        if (packet.error() == protocol::ParseError::Incomplete) {
            return std::optional<Packet>{};
        }
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse MySQL packet"));
    }

    Packet out{
        packet->sequence_id,
        std::string(packet->payload, packet->payload_len)
    };
    m_recv_ring_buffer.consume(consumed);
    return std::optional<Packet>(std::move(out));
}

std::expected<MysqlClient::Packet, MysqlError> MysqlClient::recvPacket()
{
    while (true) {
        auto parsed = tryExtractPacket();
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (parsed->has_value()) {
            return std::move(parsed->value());
        }

        auto recv_result = recvIntoRingBuffer();
        if (!recv_result) {
            return std::unexpected(recv_result.error());
        }
    }
}

MysqlResult MysqlClient::query(const std::string& sql)
{
    auto cmd = m_encoder.encodeQuery(sql, 0);
    auto send_result = sendAll(cmd);
    if (!send_result) {
        return std::unexpected(send_result.error());
    }
    return receiveResultSet();
}

MysqlBatchResult MysqlClient::batch(std::span<const protocol::MysqlCommandView> commands)
{
    std::vector<MysqlResultSet> results;
    if (commands.empty()) {
        return results;
    }

    std::vector<struct iovec> iovecs;
    iovecs.reserve(commands.size());
    for (const auto& cmd : commands) {
        if (cmd.encoded.empty()) {
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL,
                                              "Batch command encoded payload is empty"));
        }
        struct iovec iov{};
        iov.iov_base = const_cast<char*>(cmd.encoded.data());
        iov.iov_len = cmd.encoded.size();
        iovecs.push_back(iov);
    }

    auto send_result = sendAllv(std::span<const struct iovec>(iovecs.data(), iovecs.size()));
    if (!send_result) {
        return std::unexpected(send_result.error());
    }

    results.reserve(commands.size());
    for (size_t i = 0; i < commands.size(); ++i) {
        auto one = receiveResultSet();
        if (!one) {
            return std::unexpected(one.error());
        }
        results.push_back(std::move(one.value()));
    }

    return results;
}

MysqlBatchResult MysqlClient::pipeline(std::span<const std::string_view> sqls)
{
    if (sqls.empty()) {
        return std::vector<MysqlResultSet>{};
    }

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

MysqlResult MysqlClient::receiveResultSet()
{
    auto pkt_result = recvPacket();
    if (!pkt_result) {
        return std::unexpected(pkt_result.error());
    }

    auto& [seq_id, payload] = pkt_result.value();
    (void)seq_id;

    if (payload.empty()) {
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Empty server payload"));
    }

    const uint8_t first_byte = static_cast<uint8_t>(payload[0]);

    if (first_byte == 0xFF) {
        auto err = m_parser.parseErr(payload.data(), payload.size(), m_server_capabilities);
        if (err) {
            return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
        }
        return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Query error"));
    }

    if (first_byte == 0x00) {
        auto ok = m_parser.parseOk(payload.data(), payload.size(), m_server_capabilities);
        if (!ok) {
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse OK packet"));
        }

        MysqlResultSet rs;
        rs.setAffectedRows(ok->affected_rows);
        rs.setLastInsertId(ok->last_insert_id);
        rs.setWarnings(ok->warnings);
        rs.setStatusFlags(ok->status_flags);
        rs.setInfo(ok->info);
        return rs;
    }

    size_t int_consumed = 0;
    auto col_count_result = protocol::readLenEncInt(payload.data(), payload.size(), int_consumed);
    if (!col_count_result) {
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse column count"));
    }

    const uint64_t col_count = col_count_result.value();
    MysqlResultSet rs;

    for (uint64_t i = 0; i < col_count; ++i) {
        auto col_pkt = recvPacket();
        if (!col_pkt) {
            return std::unexpected(col_pkt.error());
        }

        auto& [cseq, cpayload] = col_pkt.value();
        (void)cseq;
        auto col = m_parser.parseColumnDefinition(cpayload.data(), cpayload.size());
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
        rs.addField(std::move(field));
    }

    if (!(m_server_capabilities & protocol::CLIENT_DEPRECATE_EOF)) {
        auto eof_pkt = recvPacket();
        if (!eof_pkt) {
            return std::unexpected(eof_pkt.error());
        }
    }

    while (true) {
        auto row_pkt = recvPacket();
        if (!row_pkt) {
            return std::unexpected(row_pkt.error());
        }

        auto& [rseq, rpayload] = row_pkt.value();
        (void)rseq;

        if (rpayload.empty()) {
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Empty row payload"));
        }

        const uint8_t fb = static_cast<uint8_t>(rpayload[0]);
        if (fb == 0xFE && rpayload.size() < 0xFFFFFF) {
            if (m_server_capabilities & protocol::CLIENT_DEPRECATE_EOF) {
                auto ok = m_parser.parseOk(rpayload.data(), rpayload.size(), m_server_capabilities);
                if (ok) {
                    rs.setWarnings(ok->warnings);
                    rs.setStatusFlags(ok->status_flags);
                }
            } else {
                auto eof = m_parser.parseEof(rpayload.data(), rpayload.size());
                if (eof) {
                    rs.setWarnings(eof->warnings);
                    rs.setStatusFlags(eof->status_flags);
                }
            }
            break;
        }

        if (fb == 0xFF) {
            auto err = m_parser.parseErr(rpayload.data(), rpayload.size(), m_server_capabilities);
            if (err) {
                return std::unexpected(MysqlError(MYSQL_ERROR_SERVER, err->error_code, err->error_message));
            }
            return std::unexpected(MysqlError(MYSQL_ERROR_QUERY, "Error during row fetch"));
        }

        auto row = m_parser.parseTextRow(rpayload.data(), rpayload.size(), col_count);
        if (!row) {
            return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse text row"));
        }
        rs.addRow(MysqlRow(std::move(row.value())));
    }

    return rs;
}

std::expected<MysqlClient::PrepareResult, MysqlError> MysqlClient::prepare(const std::string& sql)
{
    auto cmd = m_encoder.encodeStmtPrepare(sql, 0);
    auto send_result = sendAll(cmd);
    if (!send_result) {
        return std::unexpected(send_result.error());
    }

    auto pkt_result = recvPacket();
    if (!pkt_result) {
        return std::unexpected(pkt_result.error());
    }

    auto& [seq_id, payload] = pkt_result.value();
    (void)seq_id;

    if (payload.empty()) {
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Empty prepare response"));
    }

    if (static_cast<uint8_t>(payload[0]) == 0xFF) {
        auto err = m_parser.parseErr(payload.data(), payload.size(), m_server_capabilities);
        if (err) {
            return std::unexpected(MysqlError(MYSQL_ERROR_PREPARED_STMT,
                                              err->error_code,
                                              err->error_message));
        }
        return std::unexpected(MysqlError(MYSQL_ERROR_PREPARED_STMT, "Prepare failed"));
    }

    auto ok = m_parser.parseStmtPrepareOk(payload.data(), payload.size());
    if (!ok) {
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "Failed to parse prepare ok"));
    }

    for (uint16_t i = 0; i < ok->num_params; ++i) {
        auto p = recvPacket();
        if (!p) return std::unexpected(p.error());
    }
    if (ok->num_params > 0 && !(m_server_capabilities & protocol::CLIENT_DEPRECATE_EOF)) {
        auto eof = recvPacket();
        if (!eof) return std::unexpected(eof.error());
    }

    for (uint16_t i = 0; i < ok->num_columns; ++i) {
        auto c = recvPacket();
        if (!c) return std::unexpected(c.error());
    }
    if (ok->num_columns > 0 && !(m_server_capabilities & protocol::CLIENT_DEPRECATE_EOF)) {
        auto eof = recvPacket();
        if (!eof) return std::unexpected(eof.error());
    }

    return PrepareResult{ok->statement_id, ok->num_columns, ok->num_params};
}

MysqlResult MysqlClient::stmtExecute(uint32_t stmt_id,
                                     const std::vector<std::optional<std::string>>& params,
                                     const std::vector<uint8_t>& param_types)
{
    auto cmd = m_encoder.encodeStmtExecute(stmt_id, params, param_types, 0);
    auto send_result = sendAll(cmd);
    if (!send_result) {
        return std::unexpected(send_result.error());
    }
    return receiveResultSet();
}

MysqlVoidResult MysqlClient::stmtClose(uint32_t stmt_id)
{
    auto cmd = m_encoder.encodeStmtClose(stmt_id, 0);
    return sendAll(cmd);
}

MysqlVoidResult MysqlClient::executeSimple(const std::string& sql)
{
    auto result = query(sql);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

MysqlVoidResult MysqlClient::beginTransaction() { return executeSimple("BEGIN"); }
MysqlVoidResult MysqlClient::commit() { return executeSimple("COMMIT"); }
MysqlVoidResult MysqlClient::rollback() { return executeSimple("ROLLBACK"); }
MysqlVoidResult MysqlClient::ping() { return executeSimple("SELECT 1"); }
MysqlVoidResult MysqlClient::useDatabase(const std::string& database) { return executeSimple("USE " + database); }

void MysqlClient::close()
{
    if (!m_connected) {
        closeSocket();
        return;
    }

    auto quit = m_encoder.encodeQuit(0);
    (void)sendAll(quit);  // best effort
    closeSocket();
}

} // namespace galay::mysql
