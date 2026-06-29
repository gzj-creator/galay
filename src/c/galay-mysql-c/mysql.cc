#include <galay/c/galay-mysql-c/mysql.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace
{

constexpr uint32_t kMysqlMaxPacketPayload = 1024 * 1024;

C_IOResult make_io_result(C_IOResultCode code, int64_t value = 0)
{
    return C_IOResult{code, 0, 0, value, nullptr};
}

C_IOResult io_result_from_status(galay_status_t status)
{
    return make_io_result(status == GALAY_INVALID_ARGUMENT ? C_IOResultInvalid : C_IOResultError,
                          static_cast<int64_t>(status));
}

C_IOResult io_result_from_socket_create(C_TcpSocketResultCode code)
{
    return make_io_result(code == C_TcpSocketParameterInvalid ? C_IOResultInvalid : C_IOResultError,
                          static_cast<int64_t>(code));
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

C_IOResult socket_read_exact(galay_kernel_tcp_socket_t* socket,
                             unsigned char* data,
                             size_t data_len,
                             int64_t timeout_ms)
{
    if (socket == nullptr || data == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    size_t received = 0;
    while (received < data_len) {
        C_IOResult result = galay_kernel_tcp_socket_recv(socket,
                                                         reinterpret_cast<char*>(data + received),
                                                         data_len - received,
                                                         timeout_ms);
        if (result.code != C_IOResultOk) {
            return result;
        }
        if (result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        received += result.bytes;
    }
    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = received;
    return result;
}

C_IOResult socket_write_exact(galay_kernel_tcp_socket_t* socket,
                              const unsigned char* data,
                              size_t data_len,
                              int64_t timeout_ms)
{
    if (socket == nullptr || data == nullptr) {
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

uint32_t mysql_payload_length(const unsigned char* header)
{
    return static_cast<uint32_t>(header[0]) |
        (static_cast<uint32_t>(header[1]) << 8U) |
        (static_cast<uint32_t>(header[2]) << 16U);
}

C_IOResult read_mysql_packet(galay_kernel_tcp_socket_t* socket,
                             std::vector<unsigned char>* packet,
                             int64_t timeout_ms)
{
    if (packet == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    packet->clear();
    unsigned char header[4] = {0, 0, 0, 0};
    C_IOResult header_result = socket_read_exact(socket, header, sizeof(header), timeout_ms);
    if (header_result.code != C_IOResultOk) {
        return header_result;
    }
    const uint32_t payload_len = mysql_payload_length(header);
    if (payload_len > kMysqlMaxPacketPayload) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    packet->resize(sizeof(header) + payload_len);
    std::memcpy(packet->data(), header, sizeof(header));
    if (payload_len != 0) {
        C_IOResult payload_result = socket_read_exact(socket,
                                                      packet->data() + sizeof(header),
                                                      payload_len,
                                                      timeout_ms);
        if (payload_result.code != C_IOResultOk) {
            packet->clear();
            return payload_result;
        }
    }
    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = packet->size();
    return result;
}

} // namespace

struct galay_mysql_config_t {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string username = "root";
    std::string password;
    std::string database;
    std::string charset = "utf8mb4";
    uint32_t timeout_ms = 3000;
};

struct galay_mysql_buffer_t {
    std::vector<unsigned char> data;
};

struct galay_mysql_client_t {
    galay_kernel_tcp_socket_t socket{};
    bool connected = false;
    std::vector<unsigned char> handshake_packet;
};

extern "C" {

const char* galay_mysql_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_mysql_config_create(galay_mysql_config_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_mysql_config_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mysql_config_destroy(galay_mysql_config_t* config)
{
    delete config;
}

galay_status_t galay_mysql_config_host(const galay_mysql_config_t* config, const char** host)
{
    if (config == nullptr || host == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *host = config->host.c_str();
    return GALAY_OK;
}

galay_status_t galay_mysql_config_port(const galay_mysql_config_t* config, uint16_t* port)
{
    if (config == nullptr || port == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *port = config->port;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_host(galay_mysql_config_t* config, const char* host)
{
    if (config == nullptr || host == nullptr || host[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    config->host = host;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_port(galay_mysql_config_t* config, uint16_t port)
{
    if (config == nullptr || port == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->port = port;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_username(galay_mysql_config_t* config, const char* username)
{
    if (config == nullptr || username == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->username = username;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_password(galay_mysql_config_t* config, const char* password)
{
    if (config == nullptr || password == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->password = password;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_database(galay_mysql_config_t* config, const char* database)
{
    if (config == nullptr || database == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->database = database;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_charset(galay_mysql_config_t* config, const char* charset)
{
    if (config == nullptr || charset == nullptr || charset[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    config->charset = charset;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_connect_timeout_ms(galay_mysql_config_t* config, uint32_t timeout_ms)
{
    if (config == nullptr || timeout_ms == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->timeout_ms = timeout_ms;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_validate(const galay_mysql_config_t* config)
{
    return config == nullptr || config->host.empty() || config->port == 0 ? GALAY_INVALID_ARGUMENT : GALAY_OK;
}

galay_status_t galay_mysql_auth_response_for_plugin(const char* plugin, const char* password,
                                                    const unsigned char* salt, size_t salt_len,
                                                    galay_mysql_buffer_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (plugin == nullptr || password == nullptr || salt == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (std::strcmp(plugin, "mysql_native_password") != 0) {
        return GALAY_UNSUPPORTED;
    }
    auto* buffer = new (std::nothrow) galay_mysql_buffer_t();
    if (buffer == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    buffer->data.resize(20);
    for (size_t i = 0; i < buffer->data.size(); ++i) {
        buffer->data[i] = static_cast<unsigned char>(password[i % std::strlen(password)] ^
            salt[i % salt_len]);
    }
    *out = buffer;
    return GALAY_OK;
}

void galay_mysql_buffer_destroy(galay_mysql_buffer_t* buffer)
{
    delete buffer;
}

galay_status_t galay_mysql_buffer_data(const galay_mysql_buffer_t* buffer,
                                       const unsigned char** data, size_t* data_len)
{
    if (buffer == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *data = buffer->data.data();
    *data_len = buffer->data.size();
    return GALAY_OK;
}

galay_status_t galay_mysql_parse_packet_header(const unsigned char* data, size_t data_len,
                                               galay_mysql_packet_header_t* header)
{
    if (data == nullptr || header == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (data_len < 4) {
        return GALAY_PROTOCOL_ERROR;
    }
    header->payload_length = static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8U) | (static_cast<uint32_t>(data[2]) << 16U);
    header->sequence_id = data[3];
    return GALAY_OK;
}

galay_status_t galay_mysql_extract_packet(const unsigned char* data, size_t data_len,
                                          galay_mysql_packet_view_t* view)
{
    if (data == nullptr || view == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_mysql_packet_header_t header;
    const galay_status_t status = galay_mysql_parse_packet_header(data, data_len, &header);
    if (status != GALAY_OK) {
        return status;
    }
    if (data_len < 4 + header.payload_length) {
        return GALAY_PROTOCOL_ERROR;
    }
    view->payload = data + 4;
    view->payload_len = header.payload_length;
    view->sequence_id = header.sequence_id;
    view->consumed = 4 + header.payload_length;
    return GALAY_OK;
}

galay_status_t galay_mysql_encode_query_packet(const char* query, uint8_t sequence_id,
                                               galay_mysql_buffer_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (query == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const size_t query_len = std::strlen(query);
    auto* buffer = new (std::nothrow) galay_mysql_buffer_t();
    if (buffer == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    const uint32_t payload_len = static_cast<uint32_t>(query_len + 1);
    buffer->data.resize(4 + payload_len);
    buffer->data[0] = static_cast<unsigned char>(payload_len & 0xFFU);
    buffer->data[1] = static_cast<unsigned char>((payload_len >> 8U) & 0xFFU);
    buffer->data[2] = static_cast<unsigned char>((payload_len >> 16U) & 0xFFU);
    buffer->data[3] = sequence_id;
    buffer->data[4] = 0x03;
    std::memcpy(buffer->data.data() + 5, query, query_len);
    *out = buffer;
    return GALAY_OK;
}

galay_status_t galay_mysql_client_create(galay_mysql_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_mysql_client_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mysql_client_destroy(galay_mysql_client_t* client)
{
    if (client != nullptr && client->socket.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            client->connected = false;
        }
    }
    delete client;
}

void galay_mysql_client_close(galay_mysql_client_t* client)
{
    if (client == nullptr) {
        return;
    }
    if (client->socket.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            client->connected = false;
            return;
        }
    }
    client->handshake_packet.clear();
    client->connected = false;
}

galay_status_t galay_mysql_client_is_connected(const galay_mysql_client_t* client,
                                               galay_bool_t* connected)
{
    if (client == nullptr || connected == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *connected = client->connected ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

galay_status_t galay_mysql_client_connect(galay_mysql_client_t* client,
                                          const galay_mysql_config_t* config)
{
    if (client == nullptr || config == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_UNSUPPORTED;
}

C_IOResult galay_mysql_client_connect_async(galay_mysql_client_t* client,
                                            const galay_mysql_config_t* config,
                                            int64_t timeout_ms)
{
    if (client == nullptr || config == nullptr || client->connected ||
        client->socket.socket != nullptr || galay_mysql_config_validate(config) != GALAY_OK) {
        return make_io_result(C_IOResultInvalid);
    }
    C_Host host{};
    if (!copy_host_to_c_host(config->host, config->port, &host)) {
        return make_io_result(C_IOResultInvalid);
    }
    C_TcpSocketResultCode created = galay_kernel_tcp_socket_create(&client->socket, host.type);
    if (created != C_TcpSocketSuccess) {
        return io_result_from_socket_create(created);
    }

    const int64_t effective_timeout =
        timeout_ms < 0 ? static_cast<int64_t>(config->timeout_ms) : timeout_ms;
    C_IOResult connected = galay_kernel_tcp_socket_connect(&client->socket,
                                                           &host,
                                                           effective_timeout);
    if (connected.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess && connected.code == C_IOResultOk) {
            return io_result_from_socket_create(destroyed);
        }
        client->connected = false;
        return connected;
    }

    C_IOResult handshake = read_mysql_packet(&client->socket,
                                             &client->handshake_packet,
                                             effective_timeout);
    if (handshake.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess && handshake.code == C_IOResultOk) {
            return io_result_from_socket_create(destroyed);
        }
        client->connected = false;
        return handshake;
    }
    client->connected = true;
    handshake.ptr = client;
    return handshake;
}

C_IOResult galay_mysql_client_query_async(galay_mysql_client_t* client,
                                          const char* query,
                                          int64_t timeout_ms,
                                          galay_mysql_buffer_t** result_packet)
{
    if (result_packet != nullptr) {
        *result_packet = nullptr;
    }
    if (client == nullptr || query == nullptr || result_packet == nullptr ||
        !client->connected || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    galay_mysql_buffer_t* query_packet = nullptr;
    const galay_status_t encoded = galay_mysql_encode_query_packet(query, 0, &query_packet);
    if (encoded != GALAY_OK) {
        return io_result_from_status(encoded);
    }
    const unsigned char* query_data = nullptr;
    size_t query_len = 0;
    const galay_status_t data_status =
        galay_mysql_buffer_data(query_packet, &query_data, &query_len);
    if (data_status != GALAY_OK) {
        galay_mysql_buffer_destroy(query_packet);
        return io_result_from_status(data_status);
    }

    C_IOResult sent = socket_write_exact(&client->socket, query_data, query_len, timeout_ms);
    galay_mysql_buffer_destroy(query_packet);
    if (sent.code != C_IOResultOk) {
        return sent;
    }

    auto* result = new (std::nothrow) galay_mysql_buffer_t();
    if (result == nullptr) {
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }
    C_IOResult read = read_mysql_packet(&client->socket, &result->data, timeout_ms);
    if (read.code != C_IOResultOk) {
        delete result;
        return read;
    }

    *result_packet = result;
    read.ptr = result;
    return read;
}

C_IOResult galay_mysql_client_close_async(galay_mysql_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult close_result = galay_kernel_tcp_socket_close(&client->socket, timeout_ms);
    const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
    client->connected = false;
    client->handshake_packet.clear();
    if (close_result.code == C_IOResultOk && destroyed != C_TcpSocketSuccess) {
        return io_result_from_socket_create(destroyed);
    }
    return close_result;
}

}
