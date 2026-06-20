#include "etcd.h"

#include "../../cpp/galay-etcd/base/etcd_config.h"
#include "../../cpp/galay-etcd/base/etcd_error.h"
#include "../../cpp/galay-etcd/base/etcd_value.h"
#include "../../cpp/galay-etcd/sync/etcd_client.h"

#include <chrono>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct galay_etcd_config_builder {
    galay::etcd::EtcdConfig config;
};

struct galay_etcd_client {
    std::unique_ptr<galay::etcd::EtcdClient> client;
};

struct galay_etcd_get_result {
    std::vector<galay::etcd::EtcdKeyValue> kvs;
};

namespace {

galay_etcd_error_code_t map_etcd_error(galay::etcd::EtcdErrorType type)
{
    using galay::etcd::EtcdErrorType;
    switch (type) {
    case EtcdErrorType::Success:
        return GALAY_ETCD_ERROR_SUCCESS;
    case EtcdErrorType::InvalidEndpoint:
        return GALAY_ETCD_ERROR_INVALID_ENDPOINT;
    case EtcdErrorType::InvalidParam:
        return GALAY_ETCD_ERROR_INVALID_PARAM;
    case EtcdErrorType::NotConnected:
        return GALAY_ETCD_ERROR_NOT_CONNECTED;
    case EtcdErrorType::Connection:
        return GALAY_ETCD_ERROR_CONNECTION;
    case EtcdErrorType::Timeout:
        return GALAY_ETCD_ERROR_TIMEOUT;
    case EtcdErrorType::Send:
        return GALAY_ETCD_ERROR_SEND;
    case EtcdErrorType::Recv:
        return GALAY_ETCD_ERROR_RECV;
    case EtcdErrorType::Http:
        return GALAY_ETCD_ERROR_HTTP;
    case EtcdErrorType::Server:
        return GALAY_ETCD_ERROR_SERVER;
    case EtcdErrorType::Parse:
        return GALAY_ETCD_ERROR_PARSE;
    case EtcdErrorType::Internal:
        return GALAY_ETCD_ERROR_INTERNAL;
    default:
        return GALAY_ETCD_ERROR_UNKNOWN;
    }
}

void set_error_code(galay_etcd_error_code_t* out, galay_etcd_error_code_t code)
{
    if (out != nullptr) {
        *out = code;
    }
}

galay_status_t status_from_error(const galay::etcd::EtcdError& error,
                                 galay_etcd_error_code_t* error_code)
{
    const galay_etcd_error_code_t code = map_etcd_error(error.type());
    set_error_code(error_code, code);
    return galay_etcd_error_status(code);
}

bool invalid_c_string(const char* value)
{
    return value == nullptr || value[0] == '\0';
}

std::optional<int64_t> optional_limit(int64_t limit)
{
    return limit > 0 ? std::optional<int64_t>(limit) : std::nullopt;
}

} // namespace

extern "C" {

const char* galay_etcd_error_string(galay_etcd_error_code_t code)
{
    switch (code) {
    case GALAY_ETCD_ERROR_SUCCESS:
        return "success";
    case GALAY_ETCD_ERROR_INVALID_ENDPOINT:
        return "invalid endpoint";
    case GALAY_ETCD_ERROR_INVALID_PARAM:
        return "invalid parameter";
    case GALAY_ETCD_ERROR_NOT_CONNECTED:
        return "not connected";
    case GALAY_ETCD_ERROR_CONNECTION:
        return "connection error";
    case GALAY_ETCD_ERROR_TIMEOUT:
        return "timeout";
    case GALAY_ETCD_ERROR_SEND:
        return "send error";
    case GALAY_ETCD_ERROR_RECV:
        return "recv error";
    case GALAY_ETCD_ERROR_HTTP:
        return "http error";
    case GALAY_ETCD_ERROR_SERVER:
        return "server error";
    case GALAY_ETCD_ERROR_PARSE:
        return "parse error";
    case GALAY_ETCD_ERROR_INTERNAL:
        return "internal error";
    default:
        return "unknown error";
    }
}

galay_status_t galay_etcd_error_status(galay_etcd_error_code_t code)
{
    switch (code) {
    case GALAY_ETCD_ERROR_SUCCESS:
        return GALAY_OK;
    case GALAY_ETCD_ERROR_INVALID_ENDPOINT:
    case GALAY_ETCD_ERROR_INVALID_PARAM:
        return GALAY_INVALID_ARGUMENT;
    case GALAY_ETCD_ERROR_NOT_CONNECTED:
    case GALAY_ETCD_ERROR_CONNECTION:
    case GALAY_ETCD_ERROR_TIMEOUT:
    case GALAY_ETCD_ERROR_SEND:
    case GALAY_ETCD_ERROR_RECV:
        return GALAY_IO_ERROR;
    case GALAY_ETCD_ERROR_HTTP:
    case GALAY_ETCD_ERROR_SERVER:
    case GALAY_ETCD_ERROR_PARSE:
        return GALAY_PROTOCOL_ERROR;
    case GALAY_ETCD_ERROR_INTERNAL:
    default:
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_etcd_config_builder_create(galay_etcd_config_builder_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        *out = new (std::nothrow) galay_etcd_config_builder();
        return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_etcd_config_builder_destroy(galay_etcd_config_builder_t* builder)
{
    delete builder;
}

galay_status_t galay_etcd_config_builder_set_endpoint(galay_etcd_config_builder_t* builder,
                                                      const char* endpoint)
{
    if (builder == nullptr || invalid_c_string(endpoint)) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        builder->config.endpoint = endpoint;
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_etcd_config_builder_set_api_prefix(galay_etcd_config_builder_t* builder,
                                                        const char* api_prefix)
{
    if (builder == nullptr || invalid_c_string(api_prefix)) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        builder->config.api_prefix = api_prefix;
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_etcd_config_builder_set_request_timeout_ms(galay_etcd_config_builder_t* builder,
                                                                int64_t timeout_ms)
{
    if (builder == nullptr || timeout_ms < -1) {
        return GALAY_INVALID_ARGUMENT;
    }
    builder->config.request_timeout = std::chrono::milliseconds(timeout_ms);
    return GALAY_OK;
}

galay_status_t galay_etcd_config_builder_set_buffer_size(galay_etcd_config_builder_t* builder,
                                                         size_t buffer_size)
{
    if (builder == nullptr || buffer_size == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    builder->config.buffer_size = buffer_size;
    return GALAY_OK;
}

galay_status_t galay_etcd_config_builder_set_keepalive(galay_etcd_config_builder_t* builder,
                                                       galay_bool_t enabled)
{
    if (builder == nullptr || (enabled != GALAY_FALSE && enabled != GALAY_TRUE)) {
        return GALAY_INVALID_ARGUMENT;
    }
    builder->config.keepalive = enabled == GALAY_TRUE;
    return GALAY_OK;
}

galay_status_t galay_etcd_client_create(const galay_etcd_config_builder_t* builder,
                                        galay_etcd_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        galay::etcd::EtcdConfig config = builder == nullptr
            ? galay::etcd::EtcdConfig()
            : builder->config;
        std::unique_ptr<galay_etcd_client_t> handle(new (std::nothrow) galay_etcd_client());
        if (handle == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        handle->client = std::make_unique<galay::etcd::EtcdClient>(std::move(config));
        *out = handle.release();
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_etcd_client_destroy(galay_etcd_client_t* client)
{
    delete client;
}

galay_status_t galay_etcd_client_connect(galay_etcd_client_t* client,
                                         galay_etcd_error_code_t* error_code)
{
    set_error_code(error_code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || client->client == nullptr) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INVALID_PARAM);
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        auto result = client->client->connect();
        if (!result.has_value()) {
            return status_from_error(result.error(), error_code);
        }
        return result.value() ? GALAY_OK : GALAY_INTERNAL_ERROR;
    } catch (...) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INTERNAL);
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_etcd_client_close(galay_etcd_client_t* client,
                                       galay_etcd_error_code_t* error_code)
{
    set_error_code(error_code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || client->client == nullptr) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INVALID_PARAM);
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        auto result = client->client->close();
        if (!result.has_value()) {
            return status_from_error(result.error(), error_code);
        }
        return result.value() ? GALAY_OK : GALAY_INTERNAL_ERROR;
    } catch (...) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INTERNAL);
        return GALAY_INTERNAL_ERROR;
    }
}

galay_bool_t galay_etcd_client_connected(const galay_etcd_client_t* client)
{
    if (client == nullptr || client->client == nullptr) {
        return GALAY_FALSE;
    }
    try {
        return client->client->connected() ? GALAY_TRUE : GALAY_FALSE;
    } catch (...) {
        return GALAY_FALSE;
    }
}

galay_status_t galay_etcd_client_put(galay_etcd_client_t* client,
                                     const char* key,
                                     const void* value,
                                     size_t value_len,
                                     galay_etcd_error_code_t* error_code)
{
    set_error_code(error_code, GALAY_ETCD_ERROR_SUCCESS);
    if (client == nullptr || client->client == nullptr || invalid_c_string(key) ||
        (value == nullptr && value_len != 0)) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INVALID_PARAM);
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        std::string value_string(static_cast<const char*>(value == nullptr ? "" : value), value_len);
        auto result = client->client->put(key, value_string);
        if (!result.has_value()) {
            return status_from_error(result.error(), error_code);
        }
        return result.value() ? GALAY_OK : GALAY_INTERNAL_ERROR;
    } catch (const std::bad_alloc&) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INTERNAL);
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INTERNAL);
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_etcd_client_get(galay_etcd_client_t* client,
                                     const char* key,
                                     galay_bool_t prefix,
                                     int64_t limit,
                                     galay_etcd_get_result_t** out,
                                     galay_etcd_error_code_t* error_code)
{
    set_error_code(error_code, GALAY_ETCD_ERROR_SUCCESS);
    if (out != nullptr) {
        *out = nullptr;
    }
    if (client == nullptr || client->client == nullptr || invalid_c_string(key) || out == nullptr ||
        (prefix != GALAY_FALSE && prefix != GALAY_TRUE) || limit < 0) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INVALID_PARAM);
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        auto result = client->client->get(key, prefix == GALAY_TRUE, optional_limit(limit));
        if (!result.has_value()) {
            return status_from_error(result.error(), error_code);
        }
        std::unique_ptr<galay_etcd_get_result_t> handle(new (std::nothrow) galay_etcd_get_result());
        if (handle == nullptr) {
            set_error_code(error_code, GALAY_ETCD_ERROR_INTERNAL);
            return GALAY_OUT_OF_MEMORY;
        }
        handle->kvs = std::move(result.value());
        const bool empty = handle->kvs.empty();
        *out = handle.release();
        return empty ? GALAY_NOT_FOUND : GALAY_OK;
    } catch (const std::bad_alloc&) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INTERNAL);
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INTERNAL);
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_etcd_client_delete(galay_etcd_client_t* client,
                                        const char* key,
                                        galay_bool_t prefix,
                                        int64_t* deleted_count,
                                        galay_etcd_error_code_t* error_code)
{
    set_error_code(error_code, GALAY_ETCD_ERROR_SUCCESS);
    if (deleted_count != nullptr) {
        *deleted_count = 0;
    }
    if (client == nullptr || client->client == nullptr || invalid_c_string(key) || deleted_count == nullptr ||
        (prefix != GALAY_FALSE && prefix != GALAY_TRUE)) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INVALID_PARAM);
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        auto result = client->client->del(key, prefix == GALAY_TRUE);
        if (!result.has_value()) {
            return status_from_error(result.error(), error_code);
        }
        *deleted_count = result.value();
        return GALAY_OK;
    } catch (...) {
        set_error_code(error_code, GALAY_ETCD_ERROR_INTERNAL);
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_etcd_get_result_create_empty(galay_etcd_get_result_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        *out = new (std::nothrow) galay_etcd_get_result();
        return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_etcd_get_result_destroy(galay_etcd_get_result_t* result)
{
    delete result;
}

galay_status_t galay_etcd_get_result_count(const galay_etcd_get_result_t* result,
                                           size_t* count)
{
    if (result == nullptr || count == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *count = result->kvs.size();
    return GALAY_OK;
}

galay_status_t galay_etcd_get_result_item(const galay_etcd_get_result_t* result,
                                          size_t index,
                                          const char** key,
                                          size_t* key_len,
                                          const char** value,
                                          size_t* value_len)
{
    if (key != nullptr) {
        *key = nullptr;
    }
    if (key_len != nullptr) {
        *key_len = 0;
    }
    if (value != nullptr) {
        *value = nullptr;
    }
    if (value_len != nullptr) {
        *value_len = 0;
    }
    if (result == nullptr || key == nullptr || key_len == nullptr || value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (index >= result->kvs.size()) {
        return GALAY_NOT_FOUND;
    }
    const auto& kv = result->kvs[index];
    *key = kv.key.data();
    *key_len = kv.key.size();
    *value = kv.value.data();
    *value_len = kv.value.size();
    return GALAY_OK;
}

} // extern "C"
