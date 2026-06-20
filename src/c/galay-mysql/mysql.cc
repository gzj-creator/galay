#include <galay/c/galay-mysql/mysql.h>

#include <galay/cpp/galay-mysql/base/mysql_config.h>
#include <galay/cpp/galay-mysql/base/mysql_error.h>
#include <galay/cpp/galay-mysql/protoc/mysql_auth.h>
#include <galay/cpp/galay-mysql/protoc/mysql_protocol.h>
#include <galay/cpp/galay-mysql/sync/mysql_client.h>

#include <new>
#include <string>
#include <string_view>

struct galay_mysql_config {
    galay::mysql::MysqlConfig config;
};

struct galay_mysql_client {
    galay::mysql::MysqlClient client;
};

struct galay_mysql_buffer {
    std::string bytes;
};

namespace {

galay_status_t map_mysql_error(galay::mysql::MysqlErrorType error)
{
    using namespace galay::mysql;
    switch (error) {
    case MYSQL_ERROR_SUCCESS:
        return GALAY_OK;
    case MYSQL_ERROR_INVALID_PARAM:
        return GALAY_INVALID_ARGUMENT;
    case MYSQL_ERROR_PROTOCOL:
    case MYSQL_ERROR_AUTH:
    case MYSQL_ERROR_BUFFER_OVERFLOW:
        return GALAY_PROTOCOL_ERROR;
    case MYSQL_ERROR_CONNECTION:
    case MYSQL_ERROR_TIMEOUT:
    case MYSQL_ERROR_SEND:
    case MYSQL_ERROR_RECV:
    case MYSQL_ERROR_CONNECTION_CLOSED:
        return GALAY_IO_ERROR;
    case MYSQL_ERROR_QUERY:
    case MYSQL_ERROR_PREPARED_STMT:
    case MYSQL_ERROR_TRANSACTION:
    case MYSQL_ERROR_SERVER:
        return GALAY_PROTOCOL_ERROR;
    case MYSQL_ERROR_INTERNAL:
    default:
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t validate_config(const galay::mysql::MysqlConfig& config)
{
    if (config.host.empty() || config.port == 0 || config.username.empty() ||
        config.charset.empty() || config.connect_timeout_ms == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_OK;
}

galay_status_t set_required_string(std::string& dst, const char* value)
{
    if (value == nullptr || value[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    dst = value;
    return GALAY_OK;
}

galay_status_t make_buffer(std::string bytes, galay_mysql_buffer_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* buffer = new (std::nothrow) galay_mysql_buffer();
    if (buffer == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    buffer->bytes = std::move(bytes);
    *out = buffer;
    return GALAY_OK;
}

} // namespace

extern "C" {

galay_status_t galay_mysql_config_create(galay_mysql_config_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        auto* config = new (std::nothrow) galay_mysql_config();
        if (config == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        *out = config;
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_mysql_config_destroy(galay_mysql_config_t* config)
{
    delete config;
}

galay_status_t galay_mysql_config_set_host(galay_mysql_config_t* config, const char* host)
{
    if (config == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return set_required_string(config->config.host, host);
}

galay_status_t galay_mysql_config_set_port(galay_mysql_config_t* config, uint16_t port)
{
    if (config == nullptr || port == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->config.port = port;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_username(galay_mysql_config_t* config, const char* username)
{
    if (config == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return set_required_string(config->config.username, username);
}

galay_status_t galay_mysql_config_set_password(galay_mysql_config_t* config, const char* password)
{
    if (config == nullptr || password == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->config.password = password;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_database(galay_mysql_config_t* config, const char* database)
{
    if (config == nullptr || database == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->config.database = database;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_charset(galay_mysql_config_t* config, const char* charset)
{
    if (config == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return set_required_string(config->config.charset, charset);
}

galay_status_t galay_mysql_config_set_connect_timeout_ms(galay_mysql_config_t* config,
                                                         uint32_t timeout_ms)
{
    if (config == nullptr || timeout_ms == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->config.connect_timeout_ms = timeout_ms;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_host(const galay_mysql_config_t* config, const char** host)
{
    if (config == nullptr || host == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *host = config->config.host.c_str();
    return GALAY_OK;
}

galay_status_t galay_mysql_config_port(const galay_mysql_config_t* config, uint16_t* port)
{
    if (config == nullptr || port == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *port = config->config.port;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_validate(const galay_mysql_config_t* config)
{
    if (config == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return validate_config(config->config);
}

void galay_mysql_buffer_destroy(galay_mysql_buffer_t* buffer)
{
    delete buffer;
}

galay_status_t galay_mysql_buffer_data(const galay_mysql_buffer_t* buffer,
                                       const unsigned char** data,
                                       size_t* data_len)
{
    if (buffer == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *data = reinterpret_cast<const unsigned char*>(buffer->bytes.data());
    *data_len = buffer->bytes.size();
    return GALAY_OK;
}

galay_status_t galay_mysql_auth_response_for_plugin(const char* plugin_name,
                                                    const char* password,
                                                    const unsigned char* salt,
                                                    size_t salt_len,
                                                    galay_mysql_buffer_t** out)
{
    if (plugin_name == nullptr || password == nullptr || (salt == nullptr && salt_len != 0) ||
        out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        const char* salt_data = salt == nullptr ? "" : reinterpret_cast<const char*>(salt);
        std::string salt_string(salt_data, salt_len);
        auto result = galay::mysql::protocol::AuthPlugin::authResponseForPlugin(
            plugin_name, password, salt_string);
        if (!result) {
            return GALAY_UNSUPPORTED;
        }
        return make_buffer(std::move(result.value()), out);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mysql_parse_packet_header(const void* data,
                                               size_t data_len,
                                               galay_mysql_packet_header_t* out)
{
    if ((data == nullptr && data_len != 0) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        galay::mysql::protocol::MysqlParser parser;
        auto result = parser.parseHeader(static_cast<const char*>(data), data_len);
        if (!result) {
            return GALAY_PROTOCOL_ERROR;
        }
        out->payload_length = result->length;
        out->sequence_id = result->sequence_id;
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mysql_extract_packet(const void* data,
                                          size_t data_len,
                                          galay_mysql_packet_view_t* out)
{
    if ((data == nullptr && data_len != 0) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    out->payload = nullptr;
    out->payload_len = 0;
    out->sequence_id = 0;
    out->consumed = 0;
    try {
        size_t consumed = 0;
        galay::mysql::protocol::MysqlParser parser;
        auto result = parser.extractPacket(static_cast<const char*>(data), data_len, consumed);
        if (!result) {
            return GALAY_PROTOCOL_ERROR;
        }
        out->payload = reinterpret_cast<const unsigned char*>(result->payload);
        out->payload_len = result->payload_len;
        out->sequence_id = result->sequence_id;
        out->consumed = consumed;
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mysql_encode_query_packet(const char* sql,
                                               uint8_t sequence_id,
                                               galay_mysql_buffer_t** out)
{
    if (sql == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        galay::mysql::protocol::MysqlEncoder encoder;
        return make_buffer(encoder.encodeQuery(sql, sequence_id), out);
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_mysql_client_create(galay_mysql_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        auto* client = new (std::nothrow) galay_mysql_client();
        if (client == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        *out = client;
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_mysql_client_destroy(galay_mysql_client_t* client)
{
    delete client;
}

galay_status_t galay_mysql_client_connect(galay_mysql_client_t* client,
                                          const galay_mysql_config_t* config)
{
    if (client == nullptr || config == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_status_t status = validate_config(config->config);
    if (status != GALAY_OK) {
        return status;
    }
    try {
        auto result = client->client.connect(config->config);
        if (!result) {
            return map_mysql_error(result.error().type());
        }
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_mysql_client_close(galay_mysql_client_t* client)
{
    if (client != nullptr) {
        client->client.close();
    }
}

galay_status_t galay_mysql_client_is_connected(const galay_mysql_client_t* client,
                                               galay_bool_t* connected)
{
    if (client == nullptr || connected == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *connected = client->client.isConnected() ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

} // extern "C"
