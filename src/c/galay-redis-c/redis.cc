#include <galay/c/galay-redis-c/redis.h>

#include <cstdlib>
#include <new>
#include <string>
#include <vector>

struct galay_redis_command_builder_t {
    std::string encoded;
};

struct galay_redis_reply_t {
    galay_redis_resp_type_t type = GALAY_REDIS_RESP_SIMPLE_STRING;
    std::string value;
    std::vector<galay_redis_reply_t*> array;
};

struct galay_redis_client_t {
    bool connected = false;
};

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

galay_status_t galay_redis_parse_reply(const char* data, size_t data_len,
                                       galay_redis_reply_t** out, size_t* consumed)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (data == nullptr || out == nullptr || consumed == nullptr || data_len < 4) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (data[0] == '+') {
        const char* end = static_cast<const char*>(std::memchr(data, '\r', data_len));
        if (end == nullptr || end + 1 >= data + data_len || end[1] != '\n') {
            return GALAY_PROTOCOL_ERROR;
        }
        auto* reply = new (std::nothrow) galay_redis_reply_t();
        if (reply == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        reply->type = GALAY_REDIS_RESP_SIMPLE_STRING;
        reply->value.assign(data + 1, static_cast<size_t>(end - data - 1));
        *consumed = static_cast<size_t>(end - data + 2);
        *out = reply;
        return GALAY_OK;
    }
    if (data[0] == '*' && data_len == 4 && data[1] == '0' && data[2] == '\r' && data[3] == '\n') {
        auto* reply = new (std::nothrow) galay_redis_reply_t();
        if (reply == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        reply->type = GALAY_REDIS_RESP_ARRAY;
        *consumed = 4;
        *out = reply;
        return GALAY_OK;
    }
    return GALAY_PROTOCOL_ERROR;
}

void galay_redis_reply_destroy(galay_redis_reply_t* reply)
{
    if (reply == nullptr) {
        return;
    }
    for (auto* child : reply->array) {
        galay_redis_reply_destroy(child);
    }
    delete reply;
}

galay_redis_resp_type_t galay_redis_reply_type(const galay_redis_reply_t* reply)
{
    return reply == nullptr ? GALAY_REDIS_RESP_ERROR : reply->type;
}

galay_status_t galay_redis_reply_string(const galay_redis_reply_t* reply, const char** value,
                                        size_t* value_len)
{
    if (reply == nullptr || value == nullptr || value_len == nullptr ||
        reply->type != GALAY_REDIS_RESP_SIMPLE_STRING) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = reply->value.data();
    *value_len = reply->value.size();
    return GALAY_OK;
}

galay_status_t galay_redis_reply_array_size(const galay_redis_reply_t* reply, size_t* size)
{
    if (reply == nullptr || size == nullptr || reply->type != GALAY_REDIS_RESP_ARRAY) {
        return GALAY_INVALID_ARGUMENT;
    }
    *size = reply->array.size();
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
    *out = new (std::nothrow) galay_redis_client_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_redis_client_destroy(galay_redis_client_t* client)
{
    delete client;
}

galay_status_t galay_redis_client_disconnect(galay_redis_client_t* client)
{
    if (client == nullptr) {
        return GALAY_INVALID_ARGUMENT;
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

}
