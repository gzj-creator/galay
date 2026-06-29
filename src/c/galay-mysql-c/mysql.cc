#include <galay/c/galay-mysql-c/mysql.h>

#include <cstring>
#include <new>
#include <string>
#include <vector>

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
    bool connected = false;
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
    delete client;
}

void galay_mysql_client_close(galay_mysql_client_t* client)
{
    if (client != nullptr) {
        client->connected = false;
    }
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

}
