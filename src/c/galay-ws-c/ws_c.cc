#include <galay/c/galay-ws-c/ws_c.h>

#include <galay/cpp/galay-ws/server/ws_upgrade.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr size_t kWsMaxHttpHeaderBytes = 64 * 1024;
constexpr size_t kWsMaxFrameBufferBytes = 1024 * 1024;
constexpr const char* kClientWebSocketKey = "dGhlIHNhbXBsZSBub25jZQ==";

C_IOResult make_io_result(C_IOResultCode code, int64_t value = 0)
{
    return C_IOResult{code, 0, 0, value, nullptr};
}

C_IOResult io_result_from_status(galay_status_t status)
{
    return make_io_result(status == GALAY_INVALID_ARGUMENT ? C_IOResultInvalid : C_IOResultError,
                          static_cast<int64_t>(status));
}

C_IOResult io_result_from_ws_error(galay_ws_error_t error)
{
    return make_io_result(C_IOResultError, static_cast<int64_t>(error));
}

C_IOResult io_result_from_socket_create(C_TcpSocketResultCode code)
{
    return make_io_result(code == C_TcpSocketParameterInvalid ? C_IOResultInvalid : C_IOResultError,
                          static_cast<int64_t>(code));
}

bool valid_opcode(galay_ws_opcode_t opcode)
{
    return opcode == GALAY_WS_OPCODE_CONTINUATION || opcode == GALAY_WS_OPCODE_TEXT ||
        opcode == GALAY_WS_OPCODE_BINARY || opcode == GALAY_WS_OPCODE_CLOSE ||
        opcode == GALAY_WS_OPCODE_PING || opcode == GALAY_WS_OPCODE_PONG;
}

bool is_control_opcode(galay_ws_opcode_t opcode)
{
    return opcode == GALAY_WS_OPCODE_CLOSE || opcode == GALAY_WS_OPCODE_PING ||
        opcode == GALAY_WS_OPCODE_PONG;
}

bool is_data_opcode(galay_ws_opcode_t opcode)
{
    return opcode == GALAY_WS_OPCODE_TEXT || opcode == GALAY_WS_OPCODE_BINARY;
}

bool copy_host_to_c_host(const std::string& host, uint16_t port, C_Host* out)
{
    if (out == nullptr || host.empty() || host.size() >= sizeof(out->address) || port == 0) {
        return false;
    }
    out->type = host.find(':') == std::string::npos ? C_IPTypeIPV4 : C_IPTypeIPV6;
    std::memset(out->address, 0, sizeof(out->address));
    std::memcpy(out->address, host.data(), host.size());
    out->port = port;
    return true;
}

std::string trim_ascii_copy(std::string_view value)
{
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

char lower_ascii(char value)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool equals_ascii_ci(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (lower_ascii(left[i]) != lower_ascii(right[i])) {
            return false;
        }
    }
    return true;
}

std::string lower_ascii_copy(std::string_view value)
{
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), lower_ascii);
    return out;
}

bool contains_ascii_ci(std::string_view value, std::string_view needle)
{
    const std::string lowered_value = lower_ascii_copy(value);
    const std::string lowered_needle = lower_ascii_copy(needle);
    return lowered_value.find(lowered_needle) != std::string::npos;
}

std::string find_header_value(const std::string& headers, std::string_view name)
{
    size_t line_begin = headers.find("\r\n");
    if (line_begin == std::string::npos) {
        return {};
    }
    line_begin += 2;
    while (line_begin < headers.size()) {
        const size_t line_end = headers.find("\r\n", line_begin);
        if (line_end == std::string::npos || line_end == line_begin) {
            return {};
        }
        const size_t colon = headers.find(':', line_begin);
        if (colon != std::string::npos && colon < line_end) {
            const std::string_view key(headers.data() + line_begin, colon - line_begin);
            if (equals_ascii_ci(trim_ascii_copy(key), name)) {
                const std::string_view value(headers.data() + colon + 1, line_end - colon - 1);
                return trim_ascii_copy(value);
            }
        }
        line_begin = line_end + 2;
    }
    return {};
}

C_IOResult write_all(galay_kernel_tcp_socket_t* socket,
                     const uint8_t* data,
                     size_t data_len,
                     int64_t timeout_ms)
{
    if (socket == nullptr || socket->socket == nullptr || (data == nullptr && data_len != 0)) {
        return make_io_result(C_IOResultInvalid);
    }
    size_t sent = 0;
    while (sent < data_len) {
        C_IOResult result = galay_kernel_tcp_socket_send(socket,
                                                         reinterpret_cast<const char*>(data + sent),
                                                         data_len - sent,
                                                         timeout_ms);
        if (result.code != C_IOResultOk) {
            return result;
        }
        if (result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        sent += result.bytes;
    }
    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = sent;
    return result;
}

C_IOResult write_string(galay_kernel_tcp_socket_t* socket,
                        const std::string& data,
                        int64_t timeout_ms)
{
    return write_all(socket,
                     reinterpret_cast<const uint8_t*>(data.data()),
                     data.size(),
                     timeout_ms);
}

C_IOResult read_http_headers(galay_kernel_tcp_socket_t* socket,
                             std::string* headers,
                             size_t* header_end,
                             int64_t timeout_ms)
{
    if (socket == nullptr || socket->socket == nullptr || headers == nullptr ||
        header_end == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    headers->clear();
    *header_end = 0;
    char chunk[512];
    while (headers->size() <= kWsMaxHttpHeaderBytes) {
        const size_t found = headers->find("\r\n\r\n");
        if (found != std::string::npos) {
            *header_end = found + 4;
            C_IOResult result = make_io_result(C_IOResultOk);
            result.bytes = headers->size();
            return result;
        }

        C_IOResult recv_result =
            galay_kernel_tcp_socket_recv(socket, chunk, sizeof(chunk), timeout_ms);
        if (recv_result.code != C_IOResultOk) {
            return recv_result;
        }
        if (recv_result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        headers->append(chunk, recv_result.bytes);
    }
    return io_result_from_ws_error(GALAY_WS_ERROR_UPGRADE_FAILED);
}

bool validate_upgrade_response(const std::string& response)
{
    const bool status_ok = response.compare(0, 12, "HTTP/1.1 101") == 0 ||
        response.compare(0, 12, "HTTP/1.0 101") == 0;
    if (!status_ok) {
        return false;
    }
    const std::string accept = find_header_value(response, "Sec-WebSocket-Accept");
    const std::string expected =
        galay::websocket::WsUpgrade::generateAcceptKey(kClientWebSocketKey);
    return accept == expected;
}

bool validate_upgrade_request(const std::string& request, std::string* key)
{
    if (key == nullptr || request.compare(0, 4, "GET ") != 0) {
        return false;
    }
    const std::string upgrade = find_header_value(request, "Upgrade");
    const std::string connection = find_header_value(request, "Connection");
    const std::string version = find_header_value(request, "Sec-WebSocket-Version");
    *key = find_header_value(request, "Sec-WebSocket-Key");
    return equals_ascii_ci(upgrade, "websocket") &&
        contains_ascii_ci(connection, "upgrade") &&
        version == "13" &&
        !key->empty();
}

std::string make_upgrade_request(const std::string& host, uint16_t port, const std::string& path)
{
    std::string request = "GET ";
    request += path.empty() ? "/" : path;
    request += " HTTP/1.1\r\nHost: ";
    request += host;
    request += ":";
    request += std::to_string(port);
    request += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
    request += kClientWebSocketKey;
    request += "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    return request;
}

std::string make_upgrade_response(const std::string& key)
{
    const std::string accept = galay::websocket::WsUpgrade::generateAcceptKey(key);
    std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n";
    response += "Connection: Upgrade\r\nSec-WebSocket-Accept: ";
    response += accept;
    response += "\r\n\r\n";
    return response;
}

void append_leftover(std::vector<uint8_t>& out, const std::string& buffer, size_t header_end)
{
    if (header_end < buffer.size()) {
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(buffer.data() + header_end),
                   reinterpret_cast<const uint8_t*>(buffer.data() + buffer.size()));
    }
}

} // namespace

struct galay_ws_received_frame_t {
    galay_ws_frame_t frame{};
    std::vector<uint8_t> payload;
};

struct galay_ws_connection_t {
    galay_kernel_tcp_socket_t socket{};
    bool is_server = false;
    bool closed = false;
    bool fragmented = false;
    galay_ws_opcode_t fragmented_opcode = GALAY_WS_OPCODE_CONTINUATION;
    uint32_t mask_counter = 1;
    galay_ws_error_t last_error = GALAY_WS_ERROR_NONE;
    std::vector<uint8_t> recv_buffer;
};

struct galay_ws_session_t {
    galay_ws_connection_t* connection = nullptr;
    bool upgraded = false;
};

struct galay_ws_client_t {
    std::string host;
    uint16_t port = 0;
    std::string path;
    int connect_timeout_ms = -1;
    galay_ws_connection_t* connection = nullptr;
};

extern "C" {

const char* galay_ws_get_error(galay_ws_error_t error)
{
    switch (error) {
        case GALAY_WS_ERROR_NONE:
            return "none";
        case GALAY_WS_ERROR_INCOMPLETE:
            return "incomplete";
        case GALAY_WS_ERROR_INVALID_OPCODE:
            return "invalid opcode";
        case GALAY_WS_ERROR_MASK_REQUIRED:
            return "mask required";
        case GALAY_WS_ERROR_MASK_UNEXPECTED:
            return "mask unexpected";
        case GALAY_WS_ERROR_CONTROL_TOO_LARGE:
            return "control frame too large";
        case GALAY_WS_ERROR_CONTROL_FRAGMENTED:
            return "control frame fragmented";
        case GALAY_WS_ERROR_RESERVED_BITS:
            return "reserved bits set";
        case GALAY_WS_ERROR_INVALID_PAYLOAD_LENGTH:
            return "invalid payload length";
        case GALAY_WS_ERROR_INVALID_FRAGMENT:
            return "invalid fragment";
        case GALAY_WS_ERROR_UPGRADE_FAILED:
            return "upgrade failed";
    }
    return "unknown";
}

galay_status_t galay_ws_encoded_size(size_t payload_len, galay_bool_t masked,
                                     size_t* encoded_size)
{
    if (encoded_size == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    size_t header = 2;
    if (payload_len >= 126 && payload_len <= 65535) {
        header += 2;
    } else if (payload_len > 65535) {
        header += 8;
    }
    if (masked == GALAY_TRUE) {
        header += 4;
    }
    if (payload_len > std::numeric_limits<size_t>::max() - header) {
        return GALAY_OUT_OF_MEMORY;
    }
    *encoded_size = header + payload_len;
    return GALAY_OK;
}

galay_status_t galay_ws_apply_mask(uint8_t* data, size_t len, const uint8_t mask_key[4])
{
    if ((data == nullptr && len != 0) || mask_key == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= mask_key[i % 4];
    }
    return GALAY_OK;
}

galay_status_t galay_ws_encode_frame(galay_ws_opcode_t opcode, const uint8_t* payload,
                                     size_t payload_len, galay_bool_t fin,
                                     const uint8_t mask_key[4], uint8_t* out,
                                     size_t out_len, size_t* written)
{
    if (written != nullptr) {
        *written = 0;
    }
    if (!valid_opcode(opcode) || out == nullptr || written == nullptr ||
        (payload == nullptr && payload_len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    const bool masked = mask_key != nullptr;
    size_t need = 0;
    const galay_status_t size_status =
        galay_ws_encoded_size(payload_len, masked ? GALAY_TRUE : GALAY_FALSE, &need);
    if (size_status != GALAY_OK) {
        return size_status;
    }
    if (out_len < need) {
        return GALAY_OUT_OF_MEMORY;
    }
    size_t pos = 0;
    out[pos++] = static_cast<uint8_t>((fin == GALAY_TRUE ? 0x80U : 0U) | static_cast<uint8_t>(opcode));
    if (payload_len < 126) {
        out[pos++] = static_cast<uint8_t>((masked ? 0x80U : 0U) | payload_len);
    } else if (payload_len <= 65535) {
        out[pos++] = static_cast<uint8_t>((masked ? 0x80U : 0U) | 126U);
        out[pos++] = static_cast<uint8_t>((payload_len >> 8U) & 0xFFU);
        out[pos++] = static_cast<uint8_t>(payload_len & 0xFFU);
    } else {
        out[pos++] = static_cast<uint8_t>((masked ? 0x80U : 0U) | 127U);
        for (int i = 7; i >= 0; --i) {
            out[pos++] = static_cast<uint8_t>((static_cast<uint64_t>(payload_len) >> (i * 8)) & 0xFFU);
        }
    }
    if (masked) {
        std::memcpy(out + pos, mask_key, 4);
        pos += 4;
    }
    for (size_t i = 0; i < payload_len; ++i) {
        out[pos + i] = masked ? static_cast<uint8_t>(payload[i] ^ mask_key[i % 4]) : payload[i];
    }
    pos += payload_len;
    *written = pos;
    return GALAY_OK;
}

galay_status_t galay_ws_decode_frame(const uint8_t* data, size_t data_len,
                                     galay_bool_t expect_masked,
                                     galay_ws_frame_t* frame, uint8_t* payload_out,
                                     size_t payload_out_len, size_t* consumed,
                                     galay_ws_error_t* ws_error)
{
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (ws_error != nullptr) {
        *ws_error = GALAY_WS_ERROR_NONE;
    }
    if (data == nullptr || frame == nullptr || consumed == nullptr || ws_error == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (data_len < 2) {
        *ws_error = GALAY_WS_ERROR_INCOMPLETE;
        return GALAY_PROTOCOL_ERROR;
    }
    if ((data[0] & 0x70U) != 0) {
        *ws_error = GALAY_WS_ERROR_RESERVED_BITS;
        return GALAY_PROTOCOL_ERROR;
    }
    const bool fin = (data[0] & 0x80U) != 0;
    const auto opcode = static_cast<galay_ws_opcode_t>(data[0] & 0x0FU);
    if (!valid_opcode(opcode)) {
        *ws_error = GALAY_WS_ERROR_INVALID_OPCODE;
        return GALAY_PROTOCOL_ERROR;
    }
    size_t pos = 2;
    const bool masked = (data[1] & 0x80U) != 0;
    uint64_t payload_len = data[1] & 0x7FU;
    if (payload_len == 126) {
        if (data_len < pos + 2) {
            *ws_error = GALAY_WS_ERROR_INCOMPLETE;
            return GALAY_PROTOCOL_ERROR;
        }
        payload_len = (static_cast<uint64_t>(data[pos]) << 8U) | data[pos + 1];
        pos += 2;
    } else if (payload_len == 127) {
        if (data_len < pos + 8) {
            *ws_error = GALAY_WS_ERROR_INCOMPLETE;
            return GALAY_PROTOCOL_ERROR;
        }
        payload_len = 0;
        for (size_t i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8U) | data[pos + i];
        }
        pos += 8;
    }
    if (expect_masked == GALAY_TRUE && !masked) {
        *ws_error = GALAY_WS_ERROR_MASK_REQUIRED;
        return GALAY_PROTOCOL_ERROR;
    }
    if (expect_masked == GALAY_FALSE && masked) {
        *ws_error = GALAY_WS_ERROR_MASK_UNEXPECTED;
        return GALAY_PROTOCOL_ERROR;
    }
    if (is_control_opcode(opcode)) {
        if (!fin) {
            *ws_error = GALAY_WS_ERROR_CONTROL_FRAGMENTED;
            return GALAY_PROTOCOL_ERROR;
        }
        if (payload_len > 125) {
            *ws_error = GALAY_WS_ERROR_CONTROL_TOO_LARGE;
            return GALAY_PROTOCOL_ERROR;
        }
        if (opcode == GALAY_WS_OPCODE_CLOSE && payload_len == 1) {
            *ws_error = GALAY_WS_ERROR_INVALID_PAYLOAD_LENGTH;
            return GALAY_PROTOCOL_ERROR;
        }
    }
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (data_len < pos + 4) {
            *ws_error = GALAY_WS_ERROR_INCOMPLETE;
            return GALAY_PROTOCOL_ERROR;
        }
        std::memcpy(mask, data + pos, 4);
        pos += 4;
    }
    if (payload_len > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        *ws_error = GALAY_WS_ERROR_INVALID_PAYLOAD_LENGTH;
        return GALAY_PROTOCOL_ERROR;
    }
    const size_t payload_size = static_cast<size_t>(payload_len);
    if (payload_size > data_len - std::min(pos, data_len)) {
        *ws_error = GALAY_WS_ERROR_INCOMPLETE;
        return GALAY_PROTOCOL_ERROR;
    }
    if (payload_out_len < payload_size || (payload_out == nullptr && payload_size != 0)) {
        *ws_error = GALAY_WS_ERROR_INCOMPLETE;
        return GALAY_PROTOCOL_ERROR;
    }
    frame->fin = fin ? GALAY_TRUE : GALAY_FALSE;
    frame->opcode = opcode;
    frame->masked = masked ? GALAY_TRUE : GALAY_FALSE;
    frame->payload_len = payload_size;
    std::memcpy(frame->masking_key, mask, 4);
    for (size_t i = 0; i < payload_size; ++i) {
        payload_out[i] = masked ? static_cast<uint8_t>(data[pos + i] ^ mask[i % 4]) : data[pos + i];
    }
    *consumed = pos + payload_size;
    return GALAY_OK;
}

galay_status_t galay_ws_client_create(const galay_ws_client_config_t* config,
                                      galay_ws_client_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (config == nullptr || out == nullptr || config->host == nullptr ||
        config->host[0] == '\0' || config->path == nullptr || config->path[0] == '\0' ||
        config->port == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* client = new (std::nothrow) galay_ws_client_t();
    if (client == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    client->host = config->host;
    client->port = config->port;
    client->path = config->path;
    client->connect_timeout_ms = config->connect_timeout_ms;
    *out = client;
    return GALAY_OK;
}

void galay_ws_client_destroy(galay_ws_client_t* client)
{
    if (client == nullptr) {
        return;
    }
    if (client->connection != nullptr) {
        if (client->connection->socket.socket != nullptr) {
            const C_TcpSocketResultCode destroyed =
                galay_kernel_tcp_socket_destroy(&client->connection->socket);
            if (destroyed != C_TcpSocketSuccess) {
                client->connection->closed = true;
            }
        }
        delete client->connection;
    }
    delete client;
}

C_IOResult galay_ws_client_connect(galay_ws_client_t* client,
                                   int64_t timeout_ms,
                                   galay_ws_connection_t** out_connection)
{
    if (out_connection != nullptr) {
        *out_connection = nullptr;
    }
    if (client == nullptr || out_connection == nullptr || client->connection != nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_Host host{};
    if (!copy_host_to_c_host(client->host, client->port, &host)) {
        return make_io_result(C_IOResultInvalid);
    }
    galay_kernel_tcp_socket_t socket{};
    const C_TcpSocketResultCode created = galay_kernel_tcp_socket_create(&socket, host.type);
    if (created != C_TcpSocketSuccess) {
        return io_result_from_socket_create(created);
    }

    const int64_t effective_timeout =
        timeout_ms < 0 && client->connect_timeout_ms > 0 ? client->connect_timeout_ms : timeout_ms;
    C_IOResult connected = galay_kernel_tcp_socket_connect(&socket, &host, effective_timeout);
    if (connected.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&socket);
        if (destroyed != C_TcpSocketSuccess && connected.code == C_IOResultOk) {
            return io_result_from_socket_create(destroyed);
        }
        return connected;
    }

    const std::string request = make_upgrade_request(client->host, client->port, client->path);
    C_IOResult sent = write_string(&socket, request, effective_timeout);
    if (sent.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&socket);
        if (destroyed != C_TcpSocketSuccess && sent.code == C_IOResultOk) {
            return io_result_from_socket_create(destroyed);
        }
        return sent;
    }

    std::string response;
    size_t header_end = 0;
    C_IOResult read = read_http_headers(&socket, &response, &header_end, effective_timeout);
    if (read.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&socket);
        if (destroyed != C_TcpSocketSuccess && read.code == C_IOResultOk) {
            return io_result_from_socket_create(destroyed);
        }
        return read;
    }
    if (!validate_upgrade_response(response)) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&socket);
        if (destroyed != C_TcpSocketSuccess) {
            return io_result_from_socket_create(destroyed);
        }
        return io_result_from_ws_error(GALAY_WS_ERROR_UPGRADE_FAILED);
    }

    auto* connection = new (std::nothrow) galay_ws_connection_t();
    if (connection == nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&socket);
        if (destroyed != C_TcpSocketSuccess) {
            return io_result_from_socket_create(destroyed);
        }
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }
    connection->socket = socket;
    socket.socket = nullptr;
    connection->is_server = false;
    append_leftover(connection->recv_buffer, response, header_end);
    client->connection = connection;
    *out_connection = connection;
    read.ptr = connection;
    return read;
}

galay_status_t galay_ws_session_adopt_tcp(galay_kernel_tcp_socket_t* socket,
                                          galay_bool_t server_side,
                                          galay_ws_session_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (socket == nullptr || socket->socket == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* session = new (std::nothrow) galay_ws_session_t();
    if (session == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    auto* connection = new (std::nothrow) galay_ws_connection_t();
    if (connection == nullptr) {
        delete session;
        return GALAY_OUT_OF_MEMORY;
    }
    connection->socket = *socket;
    socket->socket = nullptr;
    connection->is_server = server_side == GALAY_TRUE;
    session->connection = connection;
    *out = session;
    return GALAY_OK;
}

void galay_ws_session_destroy(galay_ws_session_t* session)
{
    if (session == nullptr) {
        return;
    }
    if (session->connection != nullptr) {
        if (session->connection->socket.socket != nullptr) {
            const C_TcpSocketResultCode destroyed =
                galay_kernel_tcp_socket_destroy(&session->connection->socket);
            if (destroyed != C_TcpSocketSuccess) {
                session->connection->closed = true;
            }
        }
        delete session->connection;
    }
    delete session;
}

C_IOResult galay_ws_session_accept_upgrade(galay_ws_session_t* session, int64_t timeout_ms)
{
    if (session == nullptr || session->connection == nullptr ||
        !session->connection->is_server || session->upgraded) {
        return make_io_result(C_IOResultInvalid);
    }
    std::string request;
    size_t header_end = 0;
    C_IOResult read = read_http_headers(&session->connection->socket,
                                        &request,
                                        &header_end,
                                        timeout_ms);
    if (read.code != C_IOResultOk) {
        return read;
    }
    std::string key;
    if (!validate_upgrade_request(request, &key)) {
        return io_result_from_ws_error(GALAY_WS_ERROR_UPGRADE_FAILED);
    }
    append_leftover(session->connection->recv_buffer, request, header_end);
    const std::string response = make_upgrade_response(key);
    C_IOResult sent = write_string(&session->connection->socket, response, timeout_ms);
    if (sent.code != C_IOResultOk) {
        return sent;
    }
    session->upgraded = true;
    sent.ptr = session;
    return sent;
}

galay_status_t galay_ws_session_connection(galay_ws_session_t* session,
                                           galay_ws_connection_t** out_connection)
{
    if (out_connection != nullptr) {
        *out_connection = nullptr;
    }
    if (session == nullptr || out_connection == nullptr || session->connection == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_connection = session->connection;
    return GALAY_OK;
}

C_IOResult galay_ws_connection_send_frame(galay_ws_connection_t* connection,
                                          galay_ws_opcode_t opcode,
                                          const uint8_t* payload,
                                          size_t payload_len,
                                          galay_bool_t fin,
                                          int64_t timeout_ms)
{
    if (connection == nullptr || connection->socket.socket == nullptr ||
        !valid_opcode(opcode) || (payload == nullptr && payload_len != 0)) {
        return make_io_result(C_IOResultInvalid);
    }
    uint8_t mask_key[4] = {0, 0, 0, 0};
    const uint8_t* mask = nullptr;
    if (!connection->is_server) {
        const uint32_t next = connection->mask_counter++;
        mask_key[0] = static_cast<uint8_t>((next >> 24U) & 0xFFU);
        mask_key[1] = static_cast<uint8_t>((next >> 16U) & 0xFFU);
        mask_key[2] = static_cast<uint8_t>((next >> 8U) & 0xFFU);
        mask_key[3] = static_cast<uint8_t>(next & 0xFFU);
        mask = mask_key;
    }

    size_t encoded_size = 0;
    const galay_status_t size_status =
        galay_ws_encoded_size(payload_len, mask == nullptr ? GALAY_FALSE : GALAY_TRUE, &encoded_size);
    if (size_status != GALAY_OK) {
        return io_result_from_status(size_status);
    }
    std::vector<uint8_t> encoded(encoded_size == 0 ? 1 : encoded_size);
    size_t written = 0;
    const galay_status_t encoded_status = galay_ws_encode_frame(opcode,
                                                                payload,
                                                                payload_len,
                                                                fin,
                                                                mask,
                                                                encoded.data(),
                                                                encoded.size(),
                                                                &written);
    if (encoded_status != GALAY_OK) {
        return io_result_from_status(encoded_status);
    }
    C_IOResult result = write_all(&connection->socket, encoded.data(), written, timeout_ms);
    if (result.code == C_IOResultOk) {
        result.value = static_cast<int64_t>(opcode);
    }
    return result;
}

C_IOResult galay_ws_connection_send_text(galay_ws_connection_t* connection,
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         int64_t timeout_ms)
{
    return galay_ws_connection_send_frame(connection,
                                         GALAY_WS_OPCODE_TEXT,
                                         payload,
                                         payload_len,
                                         GALAY_TRUE,
                                         timeout_ms);
}

C_IOResult galay_ws_connection_send_binary(galay_ws_connection_t* connection,
                                           const uint8_t* payload,
                                           size_t payload_len,
                                           int64_t timeout_ms)
{
    return galay_ws_connection_send_frame(connection,
                                         GALAY_WS_OPCODE_BINARY,
                                         payload,
                                         payload_len,
                                         GALAY_TRUE,
                                         timeout_ms);
}

C_IOResult galay_ws_connection_send_ping(galay_ws_connection_t* connection,
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         int64_t timeout_ms)
{
    if (payload_len > 125) {
        return io_result_from_ws_error(GALAY_WS_ERROR_CONTROL_TOO_LARGE);
    }
    return galay_ws_connection_send_frame(connection,
                                         GALAY_WS_OPCODE_PING,
                                         payload,
                                         payload_len,
                                         GALAY_TRUE,
                                         timeout_ms);
}

C_IOResult galay_ws_connection_send_pong(galay_ws_connection_t* connection,
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         int64_t timeout_ms)
{
    if (payload_len > 125) {
        return io_result_from_ws_error(GALAY_WS_ERROR_CONTROL_TOO_LARGE);
    }
    return galay_ws_connection_send_frame(connection,
                                         GALAY_WS_OPCODE_PONG,
                                         payload,
                                         payload_len,
                                         GALAY_TRUE,
                                         timeout_ms);
}

C_IOResult galay_ws_connection_send_close(galay_ws_connection_t* connection,
                                          galay_ws_close_code_t close_code,
                                          const uint8_t* reason,
                                          size_t reason_len,
                                          int64_t timeout_ms)
{
    if ((reason == nullptr && reason_len != 0) || reason_len > 123) {
        return make_io_result(C_IOResultInvalid);
    }
    std::vector<uint8_t> payload(2 + reason_len);
    payload[0] = static_cast<uint8_t>((static_cast<uint16_t>(close_code) >> 8U) & 0xFFU);
    payload[1] = static_cast<uint8_t>(static_cast<uint16_t>(close_code) & 0xFFU);
    if (reason_len != 0) {
        std::memcpy(payload.data() + 2, reason, reason_len);
    }
    return galay_ws_connection_send_frame(connection,
                                         GALAY_WS_OPCODE_CLOSE,
                                         payload.data(),
                                         payload.size(),
                                         GALAY_TRUE,
                                         timeout_ms);
}

C_IOResult galay_ws_connection_recv_frame(galay_ws_connection_t* connection,
                                          int64_t timeout_ms,
                                          galay_ws_received_frame_t** out_frame)
{
    if (out_frame != nullptr) {
        *out_frame = nullptr;
    }
    if (connection == nullptr || connection->socket.socket == nullptr || out_frame == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    char chunk[4096];
    for (;;) {
        if (!connection->recv_buffer.empty()) {
            std::vector<uint8_t> payload(connection->recv_buffer.size());
            galay_ws_frame_t frame{};
            size_t consumed = 0;
            galay_ws_error_t ws_error = GALAY_WS_ERROR_NONE;
            const galay_status_t decoded =
                galay_ws_decode_frame(connection->recv_buffer.data(),
                                      connection->recv_buffer.size(),
                                      connection->is_server ? GALAY_TRUE : GALAY_FALSE,
                                      &frame,
                                      payload.data(),
                                      payload.size(),
                                      &consumed,
                                      &ws_error);
            if (decoded == GALAY_OK) {
                if (frame.opcode == GALAY_WS_OPCODE_CONTINUATION && !connection->fragmented) {
                    connection->last_error = GALAY_WS_ERROR_INVALID_FRAGMENT;
                    return io_result_from_ws_error(connection->last_error);
                }
                if (is_data_opcode(frame.opcode) && connection->fragmented) {
                    connection->last_error = GALAY_WS_ERROR_INVALID_FRAGMENT;
                    return io_result_from_ws_error(connection->last_error);
                }
                if (is_data_opcode(frame.opcode) && frame.fin == GALAY_FALSE) {
                    connection->fragmented = true;
                    connection->fragmented_opcode = frame.opcode;
                } else if (frame.opcode == GALAY_WS_OPCODE_CONTINUATION && frame.fin == GALAY_TRUE) {
                    connection->fragmented = false;
                    connection->fragmented_opcode = GALAY_WS_OPCODE_CONTINUATION;
                }

                auto* received = new (std::nothrow) galay_ws_received_frame_t();
                if (received == nullptr) {
                    return io_result_from_status(GALAY_OUT_OF_MEMORY);
                }
                received->frame = frame;
                received->payload.assign(payload.begin(), payload.begin() + frame.payload_len);
                connection->recv_buffer.erase(connection->recv_buffer.begin(),
                                              connection->recv_buffer.begin() +
                                                  static_cast<std::ptrdiff_t>(consumed));
                *out_frame = received;
                C_IOResult result = make_io_result(C_IOResultOk, static_cast<int64_t>(frame.opcode));
                result.bytes = consumed;
                result.ptr = received;
                return result;
            }
            if (decoded != GALAY_PROTOCOL_ERROR || ws_error != GALAY_WS_ERROR_INCOMPLETE) {
                connection->last_error = ws_error;
                return io_result_from_ws_error(ws_error);
            }
        }

        if (connection->recv_buffer.size() > kWsMaxFrameBufferBytes) {
            connection->last_error = GALAY_WS_ERROR_INVALID_PAYLOAD_LENGTH;
            return io_result_from_ws_error(connection->last_error);
        }
        C_IOResult recv_result =
            galay_kernel_tcp_socket_recv(&connection->socket, chunk, sizeof(chunk), timeout_ms);
        if (recv_result.code != C_IOResultOk) {
            return recv_result;
        }
        if (recv_result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        connection->recv_buffer.insert(connection->recv_buffer.end(),
                                       reinterpret_cast<uint8_t*>(chunk),
                                       reinterpret_cast<uint8_t*>(chunk) + recv_result.bytes);
    }
}

C_IOResult galay_ws_connection_close(galay_ws_connection_t* connection, int64_t timeout_ms)
{
    if (connection == nullptr || connection->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult close_result = galay_kernel_tcp_socket_close(&connection->socket, timeout_ms);
    if (close_result.code == C_IOResultOk) {
        connection->closed = true;
    }
    return close_result;
}

galay_ws_opcode_t galay_ws_received_frame_opcode(const galay_ws_received_frame_t* frame)
{
    return frame == nullptr ? GALAY_WS_OPCODE_CLOSE : frame->frame.opcode;
}

galay_bool_t galay_ws_received_frame_fin(const galay_ws_received_frame_t* frame)
{
    return frame != nullptr ? frame->frame.fin : GALAY_FALSE;
}

galay_bool_t galay_ws_received_frame_masked(const galay_ws_received_frame_t* frame)
{
    return frame != nullptr ? frame->frame.masked : GALAY_FALSE;
}

galay_status_t galay_ws_received_frame_payload(const galay_ws_received_frame_t* frame,
                                               const uint8_t** payload,
                                               size_t* payload_len)
{
    if (frame == nullptr || payload == nullptr || payload_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *payload = frame->payload.empty() ? nullptr : frame->payload.data();
    *payload_len = frame->payload.size();
    return GALAY_OK;
}

void galay_ws_received_frame_destroy(galay_ws_received_frame_t* frame)
{
    delete frame;
}

}
