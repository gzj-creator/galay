#include <galay/c/galay-redis/redis.h>

#include <galay/cpp/galay-redis/base/redis_config.h>
#include <galay/cpp/galay-redis/base/redis_error.h>
#include <galay/cpp/galay-redis/protoc/builder.h>
#include <galay/cpp/galay-redis/protoc/redis_protocol.h>
#include <galay/cpp/galay-redis/sync/redis_session.h>

#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using galay::redis::RedisCommandBuilder;
using galay::redis::RedisError;
using galay::redis::RedisErrorType;
using galay::redis::RedisSession;
using galay::redis::RedisSessionConfig;
using galay::redis::protocol::ParseError;
using galay::redis::protocol::RedisReply;
using galay::redis::protocol::RespParser;
using galay::redis::protocol::RespType;

struct galay_redis_command_builder {
    RedisCommandBuilder builder;
    std::string encoded;
};

struct galay_redis_reply {
    explicit galay_redis_reply(RedisReply value)
        : reply(std::move(value))
    {
    }

    RedisReply reply;
    mutable std::string string_cache;
    mutable std::vector<std::unique_ptr<galay_redis_reply>> element_cache;
};

struct galay_redis_client {
    explicit galay_redis_client(RedisSessionConfig cfg)
        : config(std::move(cfg))
        , session(config)
    {
    }

    RedisSessionConfig config;
    RedisSession session;
    RedisCommandBuilder builder;
};

namespace {

galay_status_t map_redis_error(const RedisError& error)
{
    using namespace galay::redis;

    switch (error.type()) {
    case REDIS_ERROR_TYPE_CONNECTION_ERROR:
    case REDIS_ERROR_TYPE_TIMEOUT_ERROR:
    case REDIS_ERROR_TYPE_SEND_ERROR:
    case REDIS_ERROR_TYPE_RECV_ERROR:
    case REDIS_ERROR_TYPE_NETWORK_ERROR:
    case REDIS_ERROR_TYPE_CONNECTION_CLOSED:
        return GALAY_IO_ERROR;
    case REDIS_ERROR_TYPE_PARSE_ERROR:
    case REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR:
    case REDIS_ERROR_TYPE_VERSION_INVALID_ERROR:
        return GALAY_PROTOCOL_ERROR;
    case REDIS_ERROR_TYPE_AUTH_ERROR:
    case REDIS_ERROR_TYPE_INVALID_ERROR:
    case REDIS_ERROR_TYPE_URL_INVALID_ERROR:
    case REDIS_ERROR_TYPE_HOST_INVALID_ERROR:
    case REDIS_ERROR_TYPE_PORT_INVALID_ERROR:
    case REDIS_ERROR_TYPE_DB_INDEX_INVALID_ERROR:
    case REDIS_ERROR_TYPE_ADDRESS_TYPE_INVALID_ERROR:
        return GALAY_INVALID_ARGUMENT;
    case REDIS_ERROR_TYPE_INTERNAL_ERROR:
    case REDIS_ERROR_TYPE_FREE_REDISOBJ_ERROR:
    case REDIS_ERROR_TYPE_UNKNOWN_ERROR:
    case REDIS_ERROR_TYPE_COMMAND_ERROR:
    default:
        return GALAY_INTERNAL_ERROR;
    }
}

galay_redis_resp_type_t from_cpp_type(RespType type)
{
    switch (type) {
    case RespType::SimpleString:
        return GALAY_REDIS_RESP_SIMPLE_STRING;
    case RespType::Error:
        return GALAY_REDIS_RESP_ERROR;
    case RespType::Integer:
        return GALAY_REDIS_RESP_INTEGER;
    case RespType::BulkString:
    case RespType::BlobError:
    case RespType::VerbatimString:
    case RespType::BigNumber:
        return GALAY_REDIS_RESP_BULK_STRING;
    case RespType::Array:
        return GALAY_REDIS_RESP_ARRAY;
    case RespType::Null:
        return GALAY_REDIS_RESP_NULL;
    case RespType::Double:
        return GALAY_REDIS_RESP_DOUBLE;
    case RespType::Boolean:
        return GALAY_REDIS_RESP_BOOLEAN;
    case RespType::Map:
        return GALAY_REDIS_RESP_MAP;
    case RespType::Set:
        return GALAY_REDIS_RESP_SET;
    case RespType::Push:
        return GALAY_REDIS_RESP_PUSH;
    default:
        return GALAY_REDIS_RESP_UNKNOWN;
    }
}

std::string_view c_string_view(const char* value, const size_t* lengths, size_t index)
{
    if (lengths == nullptr) {
        return std::string_view(value);
    }
    return std::string_view(value == nullptr ? "" : value, lengths[index]);
}

galay_status_t collect_args(const char* const* args,
                            const size_t* arg_lens,
                            size_t arg_count,
                            std::vector<std::string_view>& out)
{
    out.clear();
    if (arg_count == 0) {
        return GALAY_OK;
    }
    if (args == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    out.reserve(arg_count);
    for (size_t i = 0; i < arg_count; ++i) {
        if (args[i] == nullptr && (arg_lens == nullptr || arg_lens[i] != 0)) {
            return GALAY_INVALID_ARGUMENT;
        }
        out.push_back(c_string_view(args[i], arg_lens, i));
    }
    return GALAY_OK;
}

galay_status_t build_encoded(RedisCommandBuilder& builder,
                             const char* command,
                             const char* const* args,
                             const size_t* arg_lens,
                             size_t arg_count,
                             std::string& encoded)
{
    if (command == nullptr || command[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }

    std::vector<std::string_view> arg_views;
    galay_status_t status = collect_args(args, arg_lens, arg_count, arg_views);
    if (status != GALAY_OK) {
        return status;
    }

    auto result = builder.command(command, std::span<const std::string_view>(arg_views));
    encoded = std::move(result.encoded);
    return GALAY_OK;
}

galay_status_t validate_config(const galay_redis_client_config_t* in,
                               RedisSessionConfig& out)
{
    if (in == nullptr) {
        out = RedisSessionConfig{};
        return GALAY_OK;
    }

    const int32_t port = in->port == 0 ? 6379 : in->port;
    const int32_t version = in->resp_version == 0 ? 2 : in->resp_version;
    if (in->host != nullptr && in->host[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    if (port <= 0 || port > 65535 || in->db_index < 0 || (version != 2 && version != 3)) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (in->username != nullptr && in->username[0] != '\0' &&
        (in->password == nullptr || in->password[0] == '\0')) {
        return GALAY_INVALID_ARGUMENT;
    }

    out.host = in->host == nullptr ? "127.0.0.1" : in->host;
    out.port = port;
    out.username = in->username == nullptr ? "" : in->username;
    out.password = in->password == nullptr ? "" : in->password;
    out.db_index = in->db_index;
    out.version = version;
    out.connect_timeout_ms = in->connect_timeout_ms == 0 ? 5000U : in->connect_timeout_ms;
    return GALAY_OK;
}

const std::vector<RedisReply>& reply_array(const galay_redis_reply_t* reply)
{
    return reply->reply.asArray();
}

} // namespace

extern "C" {

galay_status_t galay_redis_command_builder_create(galay_redis_command_builder_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        *out = new (std::nothrow) galay_redis_command_builder();
        return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
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
                                                 const char** out_data,
                                                 size_t* out_len)
{
    if (builder == nullptr || out_data == nullptr || out_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_data = nullptr;
    *out_len = 0;
    try {
        galay_status_t status = build_encoded(builder->builder,
                                              command,
                                              args,
                                              arg_lens,
                                              arg_count,
                                              builder->encoded);
        if (status != GALAY_OK) {
            return status;
        }
        *out_data = builder->encoded.data();
        *out_len = builder->encoded.size();
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_redis_parse_reply(const void* data,
                                       size_t data_len,
                                       galay_redis_reply_t** out_reply,
                                       size_t* consumed)
{
    if ((data == nullptr && data_len != 0) || out_reply == nullptr || consumed == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_reply = nullptr;
    *consumed = 0;
    try {
        RespParser parser;
        RedisReply reply;
        auto result = parser.parseFast(static_cast<const char*>(data == nullptr ? "" : data),
                                       data_len,
                                       &reply);
        if (!result) {
            return result.error() == ParseError::Incomplete ? GALAY_PROTOCOL_ERROR
                                                            : GALAY_PROTOCOL_ERROR;
        }
        auto* wrapped = new (std::nothrow) galay_redis_reply(std::move(reply));
        if (wrapped == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        *consumed = result.value();
        *out_reply = wrapped;
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_redis_reply_destroy(galay_redis_reply_t* reply)
{
    delete reply;
}

galay_redis_resp_type_t galay_redis_reply_type(const galay_redis_reply_t* reply)
{
    if (reply == nullptr) {
        return GALAY_REDIS_RESP_UNKNOWN;
    }
    return from_cpp_type(reply->reply.getType());
}

galay_status_t galay_redis_reply_string(const galay_redis_reply_t* reply,
                                        const char** value,
                                        size_t* value_len)
{
    if (reply == nullptr || value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = nullptr;
    *value_len = 0;
    const galay_redis_resp_type_t type = galay_redis_reply_type(reply);
    if (type != GALAY_REDIS_RESP_SIMPLE_STRING &&
        type != GALAY_REDIS_RESP_ERROR &&
        type != GALAY_REDIS_RESP_BULK_STRING) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        reply->string_cache = reply->reply.asString();
        *value = reply->string_cache.data();
        *value_len = reply->string_cache.size();
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_redis_reply_integer(const galay_redis_reply_t* reply, int64_t* value)
{
    if (reply == nullptr || value == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (galay_redis_reply_type(reply) != GALAY_REDIS_RESP_INTEGER) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = reply->reply.asInteger();
    return GALAY_OK;
}

galay_status_t galay_redis_reply_double(const galay_redis_reply_t* reply, double* value)
{
    if (reply == nullptr || value == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (galay_redis_reply_type(reply) != GALAY_REDIS_RESP_DOUBLE) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = reply->reply.asDouble();
    return GALAY_OK;
}

galay_status_t galay_redis_reply_boolean(const galay_redis_reply_t* reply, galay_bool_t* value)
{
    if (reply == nullptr || value == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (galay_redis_reply_type(reply) != GALAY_REDIS_RESP_BOOLEAN) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = reply->reply.asBoolean() ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

galay_status_t galay_redis_reply_array_size(const galay_redis_reply_t* reply, size_t* size)
{
    if (reply == nullptr || size == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_redis_resp_type_t type = galay_redis_reply_type(reply);
    if (type != GALAY_REDIS_RESP_ARRAY && type != GALAY_REDIS_RESP_SET && type != GALAY_REDIS_RESP_PUSH) {
        return GALAY_INVALID_ARGUMENT;
    }
    *size = reply_array(reply).size();
    return GALAY_OK;
}

galay_status_t galay_redis_reply_array_get(const galay_redis_reply_t* reply,
                                           size_t index,
                                           const galay_redis_reply_t** out)
{
    if (reply == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    const auto& values = reply_array(reply);
    if (index >= values.size()) {
        return GALAY_NOT_FOUND;
    }
    try {
        if (reply->element_cache.size() != values.size()) {
            reply->element_cache.clear();
            reply->element_cache.reserve(values.size());
            for (const auto& value : values) {
                reply->element_cache.push_back(std::make_unique<galay_redis_reply>(value));
            }
        }
        *out = reply->element_cache[index].get();
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_redis_client_create(const galay_redis_client_config_t* config,
                                         galay_redis_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        RedisSessionConfig cfg;
        galay_status_t status = validate_config(config, cfg);
        if (status != GALAY_OK) {
            return status;
        }
        *out = new (std::nothrow) galay_redis_client(std::move(cfg));
        return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_redis_client_destroy(galay_redis_client_t* client)
{
    delete client;
}

galay_status_t galay_redis_client_connect(galay_redis_client_t* client)
{
    if (client == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        auto result = client->session.connect();
        return result ? GALAY_OK : map_redis_error(result.error());
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_redis_client_disconnect(galay_redis_client_t* client)
{
    if (client == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        auto result = client->session.disconnect();
        return result ? GALAY_OK : map_redis_error(result.error());
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_redis_client_command(galay_redis_client_t* client,
                                          const char* command,
                                          const char* const* args,
                                          const size_t* arg_lens,
                                          size_t arg_count,
                                          galay_redis_reply_t** out_reply)
{
    if (client == nullptr || out_reply == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_reply = nullptr;
    try {
        std::string encoded;
        galay_status_t status = build_encoded(client->builder,
                                              command,
                                              args,
                                              arg_lens,
                                              arg_count,
                                              encoded);
        if (status != GALAY_OK) {
            return status;
        }
        auto result = client->session.redisCommand(encoded);
        if (!result) {
            return map_redis_error(result.error());
        }
        auto* wrapped = new (std::nothrow) galay_redis_reply(result->getReply());
        if (wrapped == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        *out_reply = wrapped;
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

} // extern "C"
