#include <galay/c/galay-redis-c/redis_c.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr size_t kRedisMaxReplyBuffer = 1024 * 1024;

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

bool reply_may_be_incomplete(const std::string& buffer, galay_status_t status)
{
    if (status == GALAY_INVALID_ARGUMENT) {
        return true;
    }
    if (buffer.empty()) {
        return true;
    }
    if (buffer[0] == '+') {
        return buffer.find("\r\n") == std::string::npos;
    }
    if (buffer[0] == '*') {
        return buffer.size() < 4;
    }
    return false;
}

bool find_crlf(const char* data, size_t data_len, size_t offset, size_t* out)
{
    if (data == nullptr || out == nullptr || offset >= data_len) {
        return false;
    }
    for (size_t i = offset; i + 1 < data_len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            *out = i;
            return true;
        }
    }
    return false;
}

galay_status_t parse_i64(std::string_view token, int64_t* out)
{
    if (out == nullptr || token.empty()) {
        return GALAY_PROTOCOL_ERROR;
    }
    int64_t value = 0;
    const char* begin = token.data();
    const char* end = token.data() + token.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc() || parsed.ptr != end) {
        return GALAY_PROTOCOL_ERROR;
    }
    *out = value;
    return GALAY_OK;
}

galay_status_t parse_u16(std::string_view token, uint16_t* out)
{
    if (out == nullptr || token.empty()) {
        return GALAY_PROTOCOL_ERROR;
    }
    uint32_t value = 0;
    const char* begin = token.data();
    const char* end = token.data() + token.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc() || parsed.ptr != end ||
        value > std::numeric_limits<uint16_t>::max()) {
        return GALAY_PROTOCOL_ERROR;
    }
    *out = static_cast<uint16_t>(value);
    return GALAY_OK;
}

bool is_string_reply_type(galay_redis_resp_type_t type)
{
    return type == GALAY_REDIS_RESP_SIMPLE_STRING ||
        type == GALAY_REDIS_RESP_ERROR ||
        type == GALAY_REDIS_RESP_BULK_STRING ||
        type == GALAY_REDIS_RESP_BLOB_ERROR ||
        type == GALAY_REDIS_RESP_VERBATIM_STRING ||
        type == GALAY_REDIS_RESP_BIG_NUMBER;
}

bool is_array_reply_type(galay_redis_resp_type_t type)
{
    return type == GALAY_REDIS_RESP_ARRAY ||
        type == GALAY_REDIS_RESP_SET ||
        type == GALAY_REDIS_RESP_PUSH;
}

uint16_t redis_crc16(std::string_view data)
{
    uint16_t crc = 0;
    for (unsigned char byte : data) {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0) {
                crc = static_cast<uint16_t>((crc << 1U) ^ 0x1021U);
            } else {
                crc = static_cast<uint16_t>(crc << 1U);
            }
        }
    }
    return crc;
}

std::string_view redis_hash_key(std::string_view key)
{
    const size_t open = key.find('{');
    if (open == std::string_view::npos) {
        return key;
    }
    const size_t close = key.find('}', open + 1);
    if (close == std::string_view::npos || close == open + 1) {
        return key;
    }
    return key.substr(open + 1, close - open - 1);
}

} // namespace

struct galay_redis_command_builder_t {
    std::string encoded;
};

struct galay_redis_pipeline_t {
    std::vector<std::string> encoded_commands;
};

struct galay_redis_reply_t {
    galay_redis_resp_type_t type = GALAY_REDIS_RESP_SIMPLE_STRING;
    std::string value;
    int64_t integer = 0;
    double double_value = 0.0;
    galay_bool_t bool_value = GALAY_FALSE;
    std::vector<galay_redis_reply_t*> array;
    std::vector<std::pair<galay_redis_reply_t*, galay_redis_reply_t*>> map;
};

struct galay_redis_client_t {
    std::string host = "127.0.0.1";
    uint16_t port = 6379;
    std::string username;
    std::string password;
    int db_index = 0;
    int resp_version = 2;
    int connect_timeout_ms = -1;
    galay_kernel_tcp_socket_t socket{};
    bool connected = false;
    std::string recv_buffer;
};

struct galay_redis_pool_connection_t {
    galay_redis_client_t* client = nullptr;
    bool in_use = false;
};

struct galay_redis_pool_t {
    std::string host = "127.0.0.1";
    uint16_t port = 6379;
    std::string username;
    std::string password;
    int db_index = 0;
    int resp_version = 2;
    int connect_timeout_ms = -1;
    size_t min_connections = 0;
    size_t max_connections = 1;
    size_t initial_connections = 0;
    std::vector<galay_redis_pool_connection_t> connections;
};

struct galay_redis_pool_lease_t {
    galay_redis_pool_t* pool = nullptr;
    size_t index = 0;
    bool released = false;
};

struct galay_redis_cluster_node_t {
    std::string host;
    uint16_t port = 0;
    uint16_t slot_start = 0;
    uint16_t slot_end = 0;
};

struct galay_redis_cluster_t {
    std::vector<galay_redis_cluster_node_t> nodes;
    std::string last_redirect_host;
};

namespace
{

galay_status_t parse_resp_value(const char* data,
                                size_t data_len,
                                galay_redis_reply_t** out,
                                size_t* consumed);

void fill_route_from_node(const galay_redis_cluster_node_t& node,
                          size_t node_index,
                          uint16_t slot,
                          galay_redis_redirect_type_t redirect_type,
                          galay_redis_cluster_route_t* route)
{
    route->slot = slot;
    route->node_index = node_index;
    route->host = node.host.c_str();
    route->port = node.port;
    route->redirect_type = redirect_type;
}

galay_status_t parse_line_reply(const char* data,
                                size_t data_len,
                                galay_redis_resp_type_t type,
                                galay_redis_reply_t** out,
                                size_t* consumed)
{
    size_t crlf = 0;
    if (!find_crlf(data, data_len, 1, &crlf)) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* reply = new (std::nothrow) galay_redis_reply_t();
    if (reply == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    reply->type = type;
    reply->value.assign(data + 1, crlf - 1);
    *consumed = crlf + 2;
    *out = reply;
    return GALAY_OK;
}

galay_status_t parse_integer_reply(const char* data,
                                   size_t data_len,
                                   galay_redis_reply_t** out,
                                   size_t* consumed)
{
    size_t crlf = 0;
    if (!find_crlf(data, data_len, 1, &crlf)) {
        return GALAY_INVALID_ARGUMENT;
    }
    int64_t value = 0;
    const galay_status_t parsed = parse_i64(std::string_view(data + 1, crlf - 1), &value);
    if (parsed != GALAY_OK) {
        return parsed;
    }
    auto* reply = new (std::nothrow) galay_redis_reply_t();
    if (reply == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    reply->type = GALAY_REDIS_RESP_INTEGER;
    reply->integer = value;
    *consumed = crlf + 2;
    *out = reply;
    return GALAY_OK;
}

galay_status_t parse_double_reply(const char* data,
                                  size_t data_len,
                                  galay_redis_reply_t** out,
                                  size_t* consumed)
{
    size_t crlf = 0;
    if (!find_crlf(data, data_len, 1, &crlf)) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::string token(data + 1, crlf - 1);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(token.c_str(), &end);
    if (errno == ERANGE || end == token.c_str() || *end != '\0') {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* reply = new (std::nothrow) galay_redis_reply_t();
    if (reply == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    reply->type = GALAY_REDIS_RESP_DOUBLE;
    reply->double_value = value;
    *consumed = crlf + 2;
    *out = reply;
    return GALAY_OK;
}

galay_status_t parse_boolean_reply(const char* data,
                                   size_t data_len,
                                   galay_redis_reply_t** out,
                                   size_t* consumed)
{
    if (data_len < 4) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (data[2] != '\r' || data[3] != '\n' || (data[1] != 't' && data[1] != 'f')) {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* reply = new (std::nothrow) galay_redis_reply_t();
    if (reply == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    reply->type = GALAY_REDIS_RESP_BOOLEAN;
    reply->bool_value = data[1] == 't' ? GALAY_TRUE : GALAY_FALSE;
    *consumed = 4;
    *out = reply;
    return GALAY_OK;
}

galay_status_t parse_bulk_like_reply(const char* data,
                                     size_t data_len,
                                     galay_redis_resp_type_t type,
                                     galay_redis_reply_t** out,
                                     size_t* consumed)
{
    size_t crlf = 0;
    if (!find_crlf(data, data_len, 1, &crlf)) {
        return GALAY_INVALID_ARGUMENT;
    }
    int64_t len = 0;
    const galay_status_t parsed = parse_i64(std::string_view(data + 1, crlf - 1), &len);
    if (parsed != GALAY_OK) {
        return parsed;
    }
    if (len == -1 && type == GALAY_REDIS_RESP_BULK_STRING) {
        auto* reply = new (std::nothrow) galay_redis_reply_t();
        if (reply == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        reply->type = GALAY_REDIS_RESP_NIL;
        *consumed = crlf + 2;
        *out = reply;
        return GALAY_OK;
    }
    if (len < 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    const size_t body_len = static_cast<size_t>(len);
    const size_t body_start = crlf + 2;
    if (body_start > data_len || body_len > data_len - body_start ||
        body_start + body_len > data_len - 2) {
        for (size_t i = body_start; i + 1 < data_len; ++i) {
            if (data[i] == '\r' && data[i + 1] == '\n') {
                return GALAY_PROTOCOL_ERROR;
            }
        }
        return GALAY_INVALID_ARGUMENT;
    }
    const size_t body_end = body_start + body_len;
    if (data[body_end] != '\r' || data[body_end + 1] != '\n') {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* reply = new (std::nothrow) galay_redis_reply_t();
    if (reply == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    reply->type = type;
    reply->value.assign(data + body_start, body_len);
    *consumed = body_end + 2;
    *out = reply;
    return GALAY_OK;
}

galay_status_t parse_array_like_reply(const char* data,
                                      size_t data_len,
                                      galay_redis_resp_type_t type,
                                      galay_redis_reply_t** out,
                                      size_t* consumed)
{
    size_t crlf = 0;
    if (!find_crlf(data, data_len, 1, &crlf)) {
        return GALAY_INVALID_ARGUMENT;
    }
    int64_t count = 0;
    const galay_status_t parsed = parse_i64(std::string_view(data + 1, crlf - 1), &count);
    if (parsed != GALAY_OK) {
        return parsed;
    }
    if (count == -1 && type == GALAY_REDIS_RESP_ARRAY) {
        auto* reply = new (std::nothrow) galay_redis_reply_t();
        if (reply == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        reply->type = GALAY_REDIS_RESP_NIL;
        *consumed = crlf + 2;
        *out = reply;
        return GALAY_OK;
    }
    if (count < 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* reply = new (std::nothrow) galay_redis_reply_t();
    if (reply == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    reply->type = type;
    reply->array.reserve(static_cast<size_t>(count));
    size_t offset = crlf + 2;
    for (int64_t i = 0; i < count; ++i) {
        galay_redis_reply_t* child = nullptr;
        size_t child_consumed = 0;
        const galay_status_t child_status =
            parse_resp_value(data + offset, data_len - offset, &child, &child_consumed);
        if (child_status != GALAY_OK) {
            galay_redis_reply_destroy(reply);
            return child_status;
        }
        reply->array.push_back(child);
        offset += child_consumed;
    }
    *consumed = offset;
    *out = reply;
    return GALAY_OK;
}

galay_status_t parse_map_reply(const char* data,
                               size_t data_len,
                               galay_redis_reply_t** out,
                               size_t* consumed)
{
    size_t crlf = 0;
    if (!find_crlf(data, data_len, 1, &crlf)) {
        return GALAY_INVALID_ARGUMENT;
    }
    int64_t count = 0;
    const galay_status_t parsed = parse_i64(std::string_view(data + 1, crlf - 1), &count);
    if (parsed != GALAY_OK) {
        return parsed;
    }
    if (count < 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* reply = new (std::nothrow) galay_redis_reply_t();
    if (reply == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    reply->type = GALAY_REDIS_RESP_MAP;
    reply->map.reserve(static_cast<size_t>(count));
    size_t offset = crlf + 2;
    for (int64_t i = 0; i < count; ++i) {
        galay_redis_reply_t* key = nullptr;
        galay_redis_reply_t* value = nullptr;
        size_t key_consumed = 0;
        size_t value_consumed = 0;
        galay_status_t key_status =
            parse_resp_value(data + offset, data_len - offset, &key, &key_consumed);
        if (key_status != GALAY_OK) {
            galay_redis_reply_destroy(reply);
            return key_status;
        }
        offset += key_consumed;
        galay_status_t value_status =
            parse_resp_value(data + offset, data_len - offset, &value, &value_consumed);
        if (value_status != GALAY_OK) {
            galay_redis_reply_destroy(key);
            galay_redis_reply_destroy(reply);
            return value_status;
        }
        offset += value_consumed;
        reply->map.emplace_back(key, value);
    }
    *consumed = offset;
    *out = reply;
    return GALAY_OK;
}

galay_status_t parse_resp_value(const char* data,
                                size_t data_len,
                                galay_redis_reply_t** out,
                                size_t* consumed)
{
    if (data == nullptr || out == nullptr || consumed == nullptr || data_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    switch (data[0]) {
        case '+':
            return parse_line_reply(data, data_len, GALAY_REDIS_RESP_SIMPLE_STRING, out, consumed);
        case '-':
            return parse_line_reply(data, data_len, GALAY_REDIS_RESP_ERROR, out, consumed);
        case ':':
            return parse_integer_reply(data, data_len, out, consumed);
        case '$':
            return parse_bulk_like_reply(data, data_len, GALAY_REDIS_RESP_BULK_STRING, out, consumed);
        case '*':
            return parse_array_like_reply(data, data_len, GALAY_REDIS_RESP_ARRAY, out, consumed);
        case ',':
            return parse_double_reply(data, data_len, out, consumed);
        case '#':
            return parse_boolean_reply(data, data_len, out, consumed);
        case '!':
            return parse_bulk_like_reply(data, data_len, GALAY_REDIS_RESP_BLOB_ERROR, out, consumed);
        case '=':
            return parse_bulk_like_reply(data, data_len, GALAY_REDIS_RESP_VERBATIM_STRING, out, consumed);
        case '(':
            return parse_line_reply(data, data_len, GALAY_REDIS_RESP_BIG_NUMBER, out, consumed);
        case '%':
            return parse_map_reply(data, data_len, out, consumed);
        case '~':
            return parse_array_like_reply(data, data_len, GALAY_REDIS_RESP_SET, out, consumed);
        case '>':
            return parse_array_like_reply(data, data_len, GALAY_REDIS_RESP_PUSH, out, consumed);
        default:
            return GALAY_PROTOCOL_ERROR;
    }
}

galay_status_t create_pool_client(galay_redis_pool_t* pool, galay_redis_client_t** out)
{
    galay_redis_client_config_t config = {
        .host = pool->host.c_str(),
        .port = pool->port,
        .username = pool->username.empty() ? nullptr : pool->username.c_str(),
        .password = pool->password.empty() ? nullptr : pool->password.c_str(),
        .db_index = pool->db_index,
        .resp_version = pool->resp_version,
        .connect_timeout_ms = pool->connect_timeout_ms,
    };
    return galay_redis_client_create(&config, out);
}

galay_status_t parse_redirect_payload(std::string_view payload,
                                      galay_redis_redirect_type_t* type,
                                      uint16_t* slot,
                                      std::string* host,
                                      uint16_t* port)
{
    constexpr std::string_view moved_prefix = "MOVED ";
    constexpr std::string_view ask_prefix = "ASK ";
    if (payload.rfind(moved_prefix, 0) == 0) {
        *type = GALAY_REDIS_REDIRECT_MOVED;
        payload.remove_prefix(moved_prefix.size());
    } else if (payload.rfind(ask_prefix, 0) == 0) {
        *type = GALAY_REDIS_REDIRECT_ASK;
        payload.remove_prefix(ask_prefix.size());
    } else {
        return GALAY_INVALID_ARGUMENT;
    }
    const size_t space = payload.find(' ');
    if (space == std::string_view::npos || space + 1 >= payload.size()) {
        return GALAY_PROTOCOL_ERROR;
    }
    const galay_status_t slot_status = parse_u16(payload.substr(0, space), slot);
    if (slot_status != GALAY_OK || *slot > 16383U) {
        return GALAY_PROTOCOL_ERROR;
    }
    std::string_view endpoint = payload.substr(space + 1);
    const size_t colon = endpoint.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= endpoint.size()) {
        return GALAY_PROTOCOL_ERROR;
    }
    uint16_t parsed_port = 0;
    const galay_status_t port_status = parse_u16(endpoint.substr(colon + 1), &parsed_port);
    if (port_status != GALAY_OK || parsed_port == 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    host->assign(endpoint.substr(0, colon));
    *port = parsed_port;
    return host->empty() ? GALAY_PROTOCOL_ERROR : GALAY_OK;
}

} // namespace

extern "C" {

const char* galay_redis_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_redis_command_builder_create(galay_redis_command_builder_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_redis_command_builder_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_redis_command_builder_destroy(galay_redis_command_builder_t* builder)
{
    delete builder;
}

galay_status_t galay_redis_command_builder_build(galay_redis_command_builder_t* builder,
                                                 const char* command,
                                                 const char* const* args,
                                                 const size_t* arg_lens,
                                                 size_t arg_count,
                                                 const char** encoded,
                                                 size_t* encoded_len)
{
    if (encoded != nullptr) {
        *encoded = nullptr;
    }
    if (encoded_len != nullptr) {
        *encoded_len = 0;
    }
    if (builder == nullptr || command == nullptr || encoded == nullptr || encoded_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    builder->encoded = "*" + std::to_string(arg_count + 1) + "\r\n";
    const size_t command_len = std::strlen(command);
    builder->encoded += "$" + std::to_string(command_len) + "\r\n";
    builder->encoded.append(command, command_len);
    builder->encoded += "\r\n";
    for (size_t i = 0; i < arg_count; ++i) {
        if (args == nullptr || args[i] == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }
        const size_t len = arg_lens == nullptr ? std::strlen(args[i]) : arg_lens[i];
        builder->encoded += "$" + std::to_string(len) + "\r\n";
        builder->encoded.append(args[i], len);
        builder->encoded += "\r\n";
    }
    *encoded = builder->encoded.data();
    *encoded_len = builder->encoded.size();
    return GALAY_OK;
}

galay_status_t galay_redis_pipeline_create(galay_redis_pipeline_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_redis_pipeline_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_redis_pipeline_destroy(galay_redis_pipeline_t* pipeline)
{
    delete pipeline;
}

galay_status_t galay_redis_pipeline_add_command(galay_redis_pipeline_t* pipeline,
                                                const char* command,
                                                const char* const* args,
                                                const size_t* arg_lens,
                                                size_t arg_count)
{
    if (pipeline == nullptr || command == nullptr || (arg_count != 0 && args == nullptr)) {
        return GALAY_INVALID_ARGUMENT;
    }

    galay_redis_command_builder_t builder;
    const char* encoded = nullptr;
    size_t encoded_len = 0;
    galay_status_t built = galay_redis_command_builder_build(&builder,
                                                             command,
                                                             args,
                                                             arg_lens,
                                                             arg_count,
                                                             &encoded,
                                                             &encoded_len);
    if (built != GALAY_OK) {
        return built;
    }
    pipeline->encoded_commands.emplace_back(encoded, encoded_len);
    return GALAY_OK;
}

galay_status_t galay_redis_parse_reply(const char* data, size_t data_len,
                                       galay_redis_reply_t** out, size_t* consumed)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (data == nullptr || out == nullptr || consumed == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return parse_resp_value(data, data_len, out, consumed);
}

void galay_redis_reply_destroy(galay_redis_reply_t* reply)
{
    if (reply == nullptr) {
        return;
    }
    for (auto* child : reply->array) {
        galay_redis_reply_destroy(child);
    }
    for (auto& item : reply->map) {
        galay_redis_reply_destroy(item.first);
        galay_redis_reply_destroy(item.second);
    }
    delete reply;
}

void galay_redis_reply_free(galay_redis_reply_t* reply)
{
    galay_redis_reply_destroy(reply);
}

void galay_redis_pipeline_replies_destroy(galay_redis_reply_t** replies, size_t reply_count)
{
    if (replies == nullptr) {
        return;
    }
    for (size_t i = 0; i < reply_count; ++i) {
        galay_redis_reply_destroy(replies[i]);
    }
    std::free(replies);
}

galay_redis_resp_type_t galay_redis_reply_type(const galay_redis_reply_t* reply)
{
    return reply == nullptr ? GALAY_REDIS_RESP_ERROR : reply->type;
}

galay_status_t galay_redis_reply_string(const galay_redis_reply_t* reply, const char** value,
                                        size_t* value_len)
{
    if (reply == nullptr || value == nullptr || value_len == nullptr ||
        !is_string_reply_type(reply->type)) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = reply->value.data();
    *value_len = reply->value.size();
    return GALAY_OK;
}

galay_status_t galay_redis_reply_integer(const galay_redis_reply_t* reply, int64_t* value)
{
    if (reply == nullptr || value == nullptr || reply->type != GALAY_REDIS_RESP_INTEGER) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = reply->integer;
    return GALAY_OK;
}

galay_status_t galay_redis_reply_double(const galay_redis_reply_t* reply, double* value)
{
    if (reply == nullptr || value == nullptr || reply->type != GALAY_REDIS_RESP_DOUBLE) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = reply->double_value;
    return GALAY_OK;
}

galay_status_t galay_redis_reply_boolean(const galay_redis_reply_t* reply, galay_bool_t* value)
{
    if (reply == nullptr || value == nullptr || reply->type != GALAY_REDIS_RESP_BOOLEAN) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = reply->bool_value;
    return GALAY_OK;
}

galay_status_t galay_redis_reply_array_size(const galay_redis_reply_t* reply, size_t* size)
{
    if (reply == nullptr || size == nullptr || !is_array_reply_type(reply->type)) {
        return GALAY_INVALID_ARGUMENT;
    }
    *size = reply->array.size();
    return GALAY_OK;
}

galay_status_t galay_redis_reply_array_at(const galay_redis_reply_t* reply, size_t index,
                                          const galay_redis_reply_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (reply == nullptr || out == nullptr || !is_array_reply_type(reply->type)) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (index >= reply->array.size()) {
        return GALAY_NOT_FOUND;
    }
    *out = reply->array[index];
    return GALAY_OK;
}

galay_status_t galay_redis_reply_map_size(const galay_redis_reply_t* reply, size_t* size)
{
    if (reply == nullptr || size == nullptr || reply->type != GALAY_REDIS_RESP_MAP) {
        return GALAY_INVALID_ARGUMENT;
    }
    *size = reply->map.size();
    return GALAY_OK;
}

galay_status_t galay_redis_reply_map_at(const galay_redis_reply_t* reply, size_t index,
                                        const galay_redis_reply_t** key,
                                        const galay_redis_reply_t** value)
{
    if (key != nullptr) {
        *key = nullptr;
    }
    if (value != nullptr) {
        *value = nullptr;
    }
    if (reply == nullptr || key == nullptr || value == nullptr ||
        reply->type != GALAY_REDIS_RESP_MAP) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (index >= reply->map.size()) {
        return GALAY_NOT_FOUND;
    }
    *key = reply->map[index].first;
    *value = reply->map[index].second;
    return GALAY_OK;
}

galay_status_t galay_redis_client_create(const galay_redis_client_config_t* config,
                                         galay_redis_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (config != nullptr && config->username != nullptr && config->password == nullptr) {
        *out = nullptr;
        return GALAY_INVALID_ARGUMENT;
    }
    auto* client = new (std::nothrow) galay_redis_client_t();
    if (client == nullptr) {
        *out = nullptr;
        return GALAY_OUT_OF_MEMORY;
    }
    if (config != nullptr) {
        if (config->host != nullptr && config->host[0] != '\0') {
            client->host = config->host;
        }
        if (config->port != 0) {
            client->port = config->port;
        }
        if (config->username != nullptr) {
            client->username = config->username;
        }
        if (config->password != nullptr) {
            client->password = config->password;
        }
        client->db_index = config->db_index;
        client->resp_version = config->resp_version;
        client->connect_timeout_ms = config->connect_timeout_ms;
    }
    *out = client;
    return GALAY_OK;
}

void galay_redis_client_destroy(galay_redis_client_t* client)
{
    if (client != nullptr && client->socket.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            client->connected = false;
        }
    }
    delete client;
}

galay_status_t galay_redis_client_disconnect(galay_redis_client_t* client)
{
    if (client == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (client->socket.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            return GALAY_IO_ERROR;
        }
    }
    client->connected = false;
    return GALAY_OK;
}

galay_status_t galay_redis_client_command(galay_redis_client_t* client, const char* command,
                                          const char* const* args, const size_t* arg_lens,
                                          size_t arg_count, galay_redis_reply_t** reply)
{
    if (reply != nullptr) {
        *reply = nullptr;
    }
    if (client == nullptr || command == nullptr || reply == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (arg_count != 0 && args == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_UNSUPPORTED;
}

C_IOResult galay_redis_client_connect(galay_redis_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->connected || client->socket.socket != nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_Host host{};
    if (!copy_host_to_c_host(client->host, client->port, &host)) {
        return make_io_result(C_IOResultInvalid);
    }
    C_TcpSocketResultCode created = galay_kernel_tcp_socket_create(&client->socket, host.type);
    if (created != C_TcpSocketSuccess) {
        return io_result_from_socket_create(created);
    }

    const int64_t effective_timeout =
        timeout_ms < 0 && client->connect_timeout_ms > 0 ? client->connect_timeout_ms : timeout_ms;
    C_IOResult connected = galay_kernel_tcp_socket_connect(&client->socket, &host, effective_timeout);
    if (connected.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess && connected.code == C_IOResultOk) {
            return io_result_from_socket_create(destroyed);
        }
        client->connected = false;
        return connected;
    }
    client->connected = true;
    connected.ptr = client;
    return connected;
}

C_IOResult galay_redis_client_command_async(galay_redis_client_t* client,
                                            const char* command,
                                            const char* const* args,
                                            const size_t* arg_lens,
                                            size_t arg_count,
                                            int64_t timeout_ms,
                                            galay_redis_reply_t** reply)
{
    if (reply != nullptr) {
        *reply = nullptr;
    }
    if (client == nullptr || command == nullptr || reply == nullptr ||
        !client->connected || client->socket.socket == nullptr ||
        (arg_count != 0 && args == nullptr)) {
        return make_io_result(C_IOResultInvalid);
    }

    galay_redis_command_builder_t builder;
    const char* encoded = nullptr;
    size_t encoded_len = 0;
    galay_status_t built = galay_redis_command_builder_build(&builder,
                                                             command,
                                                             args,
                                                             arg_lens,
                                                             arg_count,
                                                             &encoded,
                                                             &encoded_len);
    if (built != GALAY_OK) {
        return io_result_from_status(built);
    }

    size_t sent = 0;
    while (sent < encoded_len) {
        C_IOResult send_result = galay_kernel_tcp_socket_send(&client->socket,
                                                              encoded + sent,
                                                              encoded_len - sent,
                                                              timeout_ms);
        if (send_result.code != C_IOResultOk) {
            return send_result;
        }
        if (send_result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        sent += send_result.bytes;
    }

    client->recv_buffer.clear();
    char chunk[4096];
    for (;;) {
        C_IOResult recv_result =
            galay_kernel_tcp_socket_recv(&client->socket, chunk, sizeof(chunk), timeout_ms);
        if (recv_result.code != C_IOResultOk) {
            return recv_result;
        }
        if (recv_result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        client->recv_buffer.append(chunk, recv_result.bytes);
        if (client->recv_buffer.size() > kRedisMaxReplyBuffer) {
            return io_result_from_status(GALAY_PROTOCOL_ERROR);
        }

        size_t consumed = 0;
        galay_redis_reply_t* parsed = nullptr;
        galay_status_t parsed_status = galay_redis_parse_reply(client->recv_buffer.data(),
                                                               client->recv_buffer.size(),
                                                               &parsed,
                                                               &consumed);
        if (parsed_status == GALAY_OK) {
            *reply = parsed;
            C_IOResult result = make_io_result(C_IOResultOk);
            result.bytes = consumed;
            result.ptr = parsed;
            return result;
        }
        if (!reply_may_be_incomplete(client->recv_buffer, parsed_status)) {
            return io_result_from_status(parsed_status);
        }
    }
}

C_IOResult galay_redis_client_auth(galay_redis_client_t* client,
                                   const char* username,
                                   const char* password,
                                   int64_t timeout_ms)
{
    if (client == nullptr || password == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    const bool has_username = username != nullptr && username[0] != '\0';
    const char* args[2] = {nullptr, nullptr};
    size_t arg_count = 0;
    if (has_username) {
        args[0] = username;
        args[1] = password;
        arg_count = 2;
    } else {
        args[0] = password;
        arg_count = 1;
    }

    galay_redis_reply_t* reply = nullptr;
    C_IOResult result =
        galay_redis_client_command_async(client, "AUTH", args, nullptr, arg_count, timeout_ms, &reply);
    if (result.code != C_IOResultOk) {
        return result;
    }

    const char* value = nullptr;
    size_t value_len = 0;
    const galay_status_t string_status = galay_redis_reply_string(reply, &value, &value_len);
    const bool ok = string_status == GALAY_OK && value_len == 2 &&
        std::memcmp(value, "OK", value_len) == 0;
    galay_redis_reply_destroy(reply);
    return ok ? result : io_result_from_status(GALAY_PROTOCOL_ERROR);
}

C_IOResult galay_redis_client_select(galay_redis_client_t* client, int db_index, int64_t timeout_ms)
{
    if (client == nullptr || db_index < 0) {
        return make_io_result(C_IOResultInvalid);
    }

    std::string db = std::to_string(db_index);
    const char* args[] = {db.c_str()};
    galay_redis_reply_t* reply = nullptr;
    C_IOResult result =
        galay_redis_client_command_async(client, "SELECT", args, nullptr, 1, timeout_ms, &reply);
    if (result.code != C_IOResultOk) {
        return result;
    }

    const char* value = nullptr;
    size_t value_len = 0;
    const galay_status_t string_status = galay_redis_reply_string(reply, &value, &value_len);
    const bool ok = string_status == GALAY_OK && value_len == 2 &&
        std::memcmp(value, "OK", value_len) == 0;
    galay_redis_reply_destroy(reply);
    return ok ? result : io_result_from_status(GALAY_PROTOCOL_ERROR);
}

C_IOResult galay_redis_client_pipeline_async(galay_redis_client_t* client,
                                             const galay_redis_pipeline_t* pipeline,
                                             int64_t timeout_ms,
                                             galay_redis_reply_t*** replies,
                                             size_t* reply_count)
{
    if (replies != nullptr) {
        *replies = nullptr;
    }
    if (reply_count != nullptr) {
        *reply_count = 0;
    }
    if (client == nullptr || pipeline == nullptr || replies == nullptr || reply_count == nullptr ||
        !client->connected || client->socket.socket == nullptr ||
        pipeline->encoded_commands.empty()) {
        return make_io_result(C_IOResultInvalid);
    }

    for (const std::string& encoded : pipeline->encoded_commands) {
        size_t sent = 0;
        while (sent < encoded.size()) {
            C_IOResult send_result = galay_kernel_tcp_socket_send(&client->socket,
                                                                  encoded.data() + sent,
                                                                  encoded.size() - sent,
                                                                  timeout_ms);
            if (send_result.code != C_IOResultOk) {
                return send_result;
            }
            if (send_result.bytes == 0) {
                return make_io_result(C_IOResultEof);
            }
            sent += send_result.bytes;
        }
    }

    const size_t expected_count = pipeline->encoded_commands.size();
    auto** parsed_replies =
        static_cast<galay_redis_reply_t**>(std::calloc(expected_count,
                                                       sizeof(galay_redis_reply_t*)));
    if (parsed_replies == nullptr) {
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }

    client->recv_buffer.clear();
    char chunk[4096];
    size_t parsed_count = 0;
    while (parsed_count < expected_count) {
        size_t consumed = 0;
        galay_redis_reply_t* parsed = nullptr;
        galay_status_t parsed_status = galay_redis_parse_reply(client->recv_buffer.data(),
                                                               client->recv_buffer.size(),
                                                               &parsed,
                                                               &consumed);
        if (parsed_status == GALAY_OK) {
            parsed_replies[parsed_count] = parsed;
            ++parsed_count;
            client->recv_buffer.erase(0, consumed);
            continue;
        }
        if (!reply_may_be_incomplete(client->recv_buffer, parsed_status)) {
            galay_redis_pipeline_replies_destroy(parsed_replies, expected_count);
            return io_result_from_status(parsed_status);
        }

        C_IOResult recv_result =
            galay_kernel_tcp_socket_recv(&client->socket, chunk, sizeof(chunk), timeout_ms);
        if (recv_result.code != C_IOResultOk) {
            galay_redis_pipeline_replies_destroy(parsed_replies, expected_count);
            return recv_result;
        }
        if (recv_result.bytes == 0) {
            galay_redis_pipeline_replies_destroy(parsed_replies, expected_count);
            return make_io_result(C_IOResultEof);
        }
        client->recv_buffer.append(chunk, recv_result.bytes);
        if (client->recv_buffer.size() > kRedisMaxReplyBuffer) {
            galay_redis_pipeline_replies_destroy(parsed_replies, expected_count);
            return io_result_from_status(GALAY_PROTOCOL_ERROR);
        }
    }

    *replies = parsed_replies;
    *reply_count = expected_count;
    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = expected_count;
    result.ptr = parsed_replies;
    return result;
}

galay_status_t galay_redis_pool_create(const galay_redis_pool_config_t* config,
                                       galay_redis_pool_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* pool = new (std::nothrow) galay_redis_pool_t();
    if (pool == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }

    if (config != nullptr) {
        if (config->max_connections == 0 ||
            config->min_connections > config->max_connections ||
            config->initial_connections > config->max_connections ||
            (config->client.username != nullptr && config->client.password == nullptr)) {
            delete pool;
            return GALAY_INVALID_ARGUMENT;
        }
        if (config->client.host != nullptr && config->client.host[0] != '\0') {
            pool->host = config->client.host;
        }
        if (config->client.port != 0) {
            pool->port = config->client.port;
        }
        if (config->client.username != nullptr) {
            pool->username = config->client.username;
        }
        if (config->client.password != nullptr) {
            pool->password = config->client.password;
        }
        pool->db_index = config->client.db_index;
        pool->resp_version = config->client.resp_version;
        pool->connect_timeout_ms = config->client.connect_timeout_ms;
        pool->min_connections = config->min_connections;
        pool->max_connections = config->max_connections;
        pool->initial_connections = config->initial_connections;
    }

    pool->connections.reserve(pool->max_connections);
    for (size_t i = 0; i < pool->initial_connections; ++i) {
        galay_redis_client_t* client = nullptr;
        const galay_status_t created = create_pool_client(pool, &client);
        if (created != GALAY_OK) {
            for (auto& connection : pool->connections) {
                if (connection.client != nullptr) {
                    galay_redis_client_destroy(connection.client);
                }
            }
            delete pool;
            return created;
        }
        pool->connections.push_back(galay_redis_pool_connection_t{client, false});
    }
    *out = pool;
    return GALAY_OK;
}

void galay_redis_pool_destroy(galay_redis_pool_t* pool)
{
    if (pool == nullptr) {
        return;
    }
    for (auto& connection : pool->connections) {
        if (connection.client != nullptr) {
            galay_redis_client_destroy(connection.client);
        }
    }
    delete pool;
}

C_IOResult galay_redis_pool_acquire(galay_redis_pool_t* pool,
                                    int64_t timeout_ms,
                                    galay_redis_pool_lease_t** lease)
{
    if (lease != nullptr) {
        *lease = nullptr;
    }
    if (pool == nullptr || lease == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    size_t index = pool->connections.size();
    for (size_t i = 0; i < pool->connections.size(); ++i) {
        if (!pool->connections[i].in_use) {
            index = i;
            break;
        }
    }

    if (index == pool->connections.size()) {
        if (pool->connections.size() >= pool->max_connections) {
            return make_io_result(C_IOResultTimeout);
        }
        galay_redis_client_t* client = nullptr;
        const galay_status_t created = create_pool_client(pool, &client);
        if (created != GALAY_OK) {
            return io_result_from_status(created);
        }
        pool->connections.push_back(galay_redis_pool_connection_t{client, false});
    }

    auto& connection = pool->connections[index];
    connection.in_use = true;
    if (!connection.client->connected) {
        C_IOResult connected = galay_redis_client_connect(connection.client, timeout_ms);
        if (connected.code != C_IOResultOk) {
            connection.in_use = false;
            if (index + 1 == pool->connections.size() &&
                pool->connections.size() > pool->initial_connections) {
                galay_redis_client_destroy(connection.client);
                pool->connections.pop_back();
            }
            return connected;
        }
    }

    auto* new_lease = new (std::nothrow) galay_redis_pool_lease_t();
    if (new_lease == nullptr) {
        connection.in_use = false;
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }
    new_lease->pool = pool;
    new_lease->index = index;
    *lease = new_lease;
    C_IOResult result = make_io_result(C_IOResultOk);
    result.ptr = new_lease;
    return result;
}

galay_status_t galay_redis_pool_release(galay_redis_pool_t* pool,
                                        galay_redis_pool_lease_t* lease)
{
    if (pool == nullptr || lease == nullptr || lease->pool != pool || lease->released ||
        lease->index >= pool->connections.size() || !pool->connections[lease->index].in_use) {
        return GALAY_INVALID_ARGUMENT;
    }
    pool->connections[lease->index].in_use = false;
    lease->released = true;
    delete lease;
    return GALAY_OK;
}

galay_redis_client_t* galay_redis_pool_lease_client(galay_redis_pool_lease_t* lease)
{
    if (lease == nullptr || lease->released || lease->pool == nullptr ||
        lease->index >= lease->pool->connections.size()) {
        return nullptr;
    }
    return lease->pool->connections[lease->index].client;
}

galay_status_t galay_redis_cluster_create(galay_redis_cluster_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_redis_cluster_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_redis_cluster_destroy(galay_redis_cluster_t* cluster)
{
    delete cluster;
}

galay_status_t galay_redis_cluster_add_node(galay_redis_cluster_t* cluster,
                                            const galay_redis_cluster_node_config_t* node)
{
    if (cluster == nullptr || node == nullptr || node->host == nullptr ||
        node->host[0] == '\0' || node->port == 0 ||
        node->slot_start > node->slot_end || node->slot_end > 16383U) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_redis_cluster_node_t stored;
    stored.host = node->host;
    stored.port = node->port;
    stored.slot_start = node->slot_start;
    stored.slot_end = node->slot_end;
    cluster->nodes.push_back(std::move(stored));
    return GALAY_OK;
}

galay_status_t galay_redis_cluster_key_slot(const char* key, size_t key_len, uint16_t* slot)
{
    if (key == nullptr || slot == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::string_view key_view(key, key_len);
    *slot = static_cast<uint16_t>(redis_crc16(redis_hash_key(key_view)) % 16384U);
    return GALAY_OK;
}

galay_status_t galay_redis_cluster_route_slot(const galay_redis_cluster_t* cluster,
                                              uint16_t slot,
                                              galay_redis_cluster_route_t* route)
{
    if (route != nullptr) {
        *route = galay_redis_cluster_route_t{};
    }
    if (cluster == nullptr || route == nullptr || slot > 16383U) {
        return GALAY_INVALID_ARGUMENT;
    }
    for (size_t i = cluster->nodes.size(); i > 0; --i) {
        const size_t index = i - 1;
        const auto& node = cluster->nodes[index];
        if (slot >= node.slot_start && slot <= node.slot_end) {
            fill_route_from_node(node, index, slot, GALAY_REDIS_REDIRECT_NONE, route);
            return GALAY_OK;
        }
    }
    return GALAY_NOT_FOUND;
}

galay_status_t galay_redis_cluster_route_key(const galay_redis_cluster_t* cluster,
                                             const char* key,
                                             size_t key_len,
                                             galay_redis_cluster_route_t* route)
{
    uint16_t slot = 0;
    const galay_status_t slot_status = galay_redis_cluster_key_slot(key, key_len, &slot);
    if (slot_status != GALAY_OK) {
        return slot_status;
    }
    return galay_redis_cluster_route_slot(cluster, slot, route);
}

galay_status_t galay_redis_cluster_apply_redirect(galay_redis_cluster_t* cluster,
                                                  const galay_redis_reply_t* reply,
                                                  galay_redis_cluster_route_t* route)
{
    if (route != nullptr) {
        *route = galay_redis_cluster_route_t{};
    }
    if (cluster == nullptr || reply == nullptr || route == nullptr ||
        reply->type != GALAY_REDIS_RESP_ERROR) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_redis_redirect_type_t redirect_type = GALAY_REDIS_REDIRECT_NONE;
    uint16_t slot = 0;
    uint16_t port = 0;
    std::string host;
    const galay_status_t parsed =
        parse_redirect_payload(reply->value, &redirect_type, &slot, &host, &port);
    if (parsed != GALAY_OK) {
        return parsed;
    }
    if (redirect_type == GALAY_REDIS_REDIRECT_MOVED) {
        galay_redis_cluster_node_config_t node = {
            .host = host.c_str(),
            .port = port,
            .slot_start = slot,
            .slot_end = slot,
        };
        const galay_status_t added = galay_redis_cluster_add_node(cluster, &node);
        if (added != GALAY_OK) {
            return added;
        }
        fill_route_from_node(cluster->nodes.back(),
                             cluster->nodes.size() - 1,
                             slot,
                             GALAY_REDIS_REDIRECT_MOVED,
                             route);
        return GALAY_OK;
    }
    cluster->last_redirect_host = std::move(host);
    route->slot = slot;
    route->node_index = std::numeric_limits<size_t>::max();
    route->host = cluster->last_redirect_host.c_str();
    route->port = port;
    route->redirect_type = GALAY_REDIS_REDIRECT_ASK;
    return GALAY_OK;
}

C_IOResult galay_redis_client_close(galay_redis_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    C_IOResult close_result = galay_kernel_tcp_socket_close(&client->socket, timeout_ms);
    const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
    client->connected = false;
    client->recv_buffer.clear();
    if (close_result.code == C_IOResultOk && destroyed != C_TcpSocketSuccess) {
        return io_result_from_socket_create(destroyed);
    }
    return close_result;
}

}
