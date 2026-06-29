#include <galay/c/galay-etcd-c/etcd.h>

#include <new>
#include <string>

struct galay_etcd_config_builder_t {
    std::string endpoint = "http://127.0.0.1:2379";
};

struct galay_etcd_client_t {
    std::string endpoint;
};

struct galay_etcd_get_result_t {
    size_t count = 0;
};

static bool valid_endpoint(const std::string& endpoint)
{
    return endpoint.rfind("http://", 0) == 0 || endpoint.rfind("https://", 0) == 0;
}

extern "C" {

const char* galay_etcd_error_string(galay_etcd_error_code_t code)
{
    switch (code) {
        case GALAY_ETCD_ERROR_SUCCESS:
            return "success";
        case GALAY_ETCD_ERROR_INVALID_ENDPOINT:
            return "invalid endpoint";
    }
    return "unknown";
}

galay_status_t galay_etcd_error_status(galay_etcd_error_code_t code)
{
    return code == GALAY_ETCD_ERROR_SUCCESS ? GALAY_OK : GALAY_INVALID_ARGUMENT;
}

galay_status_t galay_etcd_config_builder_create(galay_etcd_config_builder_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_etcd_config_builder_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_etcd_config_builder_destroy(galay_etcd_config_builder_t* builder)
{
    delete builder;
}

galay_status_t galay_etcd_config_builder_set_endpoint(galay_etcd_config_builder_t* builder,
                                                      const char* endpoint)
{
    if (builder == nullptr || endpoint == nullptr || endpoint[0] == '\0') return GALAY_INVALID_ARGUMENT;
    builder->endpoint = endpoint;
    return GALAY_OK;
}

galay_status_t galay_etcd_client_create(const galay_etcd_config_builder_t* builder,
                                        galay_etcd_client_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    auto* client = new (std::nothrow) galay_etcd_client_t();
    if (client == nullptr) return GALAY_OUT_OF_MEMORY;
    client->endpoint = builder == nullptr ? "http://127.0.0.1:2379" : builder->endpoint;
    *out = client;
    return GALAY_OK;
}

void galay_etcd_client_destroy(galay_etcd_client_t* client)
{
    delete client;
}

galay_status_t galay_etcd_client_connect(galay_etcd_client_t* client,
                                         galay_etcd_error_code_t* code)
{
    if (code != nullptr) *code = GALAY_ETCD_ERROR_SUCCESS;
    if (client == nullptr) return GALAY_INVALID_ARGUMENT;
    if (!valid_endpoint(client->endpoint)) {
        if (code != nullptr) *code = GALAY_ETCD_ERROR_INVALID_ENDPOINT;
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_UNSUPPORTED;
}

galay_status_t galay_etcd_client_put(galay_etcd_client_t* client, const char* key,
                                     const char* value, size_t value_len,
                                     galay_etcd_error_code_t* code)
{
    if (code != nullptr) *code = GALAY_ETCD_ERROR_SUCCESS;
    if (client == nullptr || key == nullptr || key[0] == '\0' ||
        (value == nullptr && value_len != 0)) return GALAY_INVALID_ARGUMENT;
    return GALAY_UNSUPPORTED;
}

galay_status_t galay_etcd_client_get(galay_etcd_client_t* client, const char* key,
                                     galay_bool_t prefix, int64_t limit,
                                     galay_etcd_get_result_t** result,
                                     galay_etcd_error_code_t* code)
{
    if (result != nullptr) *result = nullptr;
    if (code != nullptr) *code = GALAY_ETCD_ERROR_SUCCESS;
    if (client == nullptr || key == nullptr || key[0] == '\0' || result == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return prefix == GALAY_TRUE || limit != 0 ? GALAY_UNSUPPORTED : GALAY_UNSUPPORTED;
}

galay_status_t galay_etcd_client_delete(galay_etcd_client_t* client, const char* key,
                                        galay_bool_t prefix, int64_t* deleted_count,
                                        galay_etcd_error_code_t* code)
{
    if (deleted_count != nullptr) *deleted_count = 0;
    if (code != nullptr) *code = GALAY_ETCD_ERROR_SUCCESS;
    if (client == nullptr || key == nullptr || key[0] == '\0') return GALAY_INVALID_ARGUMENT;
    return prefix == GALAY_TRUE ? GALAY_UNSUPPORTED : GALAY_UNSUPPORTED;
}

galay_status_t galay_etcd_get_result_create_empty(galay_etcd_get_result_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_etcd_get_result_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_etcd_get_result_destroy(galay_etcd_get_result_t* result)
{
    delete result;
}

galay_status_t galay_etcd_get_result_count(const galay_etcd_get_result_t* result, size_t* count)
{
    if (result == nullptr || count == nullptr) return GALAY_INVALID_ARGUMENT;
    *count = result->count;
    return GALAY_OK;
}

galay_status_t galay_etcd_get_result_item(const galay_etcd_get_result_t* result, size_t index,
                                          const char** key, size_t* key_len,
                                          const char** value, size_t* value_len)
{
    if (key != nullptr) *key = nullptr;
    if (key_len != nullptr) *key_len = 0;
    if (value != nullptr) *value = nullptr;
    if (value_len != nullptr) *value_len = 0;
    if (result == nullptr || key == nullptr || key_len == nullptr || value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return index >= result->count ? GALAY_NOT_FOUND : GALAY_INTERNAL_ERROR;
}

}
