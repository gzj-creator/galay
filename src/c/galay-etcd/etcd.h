/**
 * @file etcd.h
 * @brief galay-etcd C ABI 封装。
 *
 * @details C API 只暴露 opaque handle、稳定错误枚举和显式 create/destroy。
 *          get result 中返回的 key/value 指针由 result 持有，在 result destroy 前有效。
 */

#ifndef GALAY_C_ETCD_ETCD_H
#define GALAY_C_ETCD_ETCD_H

#include <galay/c/galay-c/common/galay_c_error.h>

GALAY_C_BEGIN_DECLS

typedef struct galay_etcd_config_builder galay_etcd_config_builder_t;
typedef struct galay_etcd_client galay_etcd_client_t;
typedef struct galay_etcd_get_result galay_etcd_get_result_t;

typedef enum galay_etcd_error_code {
    GALAY_ETCD_ERROR_SUCCESS = 0,
    GALAY_ETCD_ERROR_INVALID_ENDPOINT = 1,
    GALAY_ETCD_ERROR_INVALID_PARAM = 2,
    GALAY_ETCD_ERROR_NOT_CONNECTED = 3,
    GALAY_ETCD_ERROR_CONNECTION = 4,
    GALAY_ETCD_ERROR_TIMEOUT = 5,
    GALAY_ETCD_ERROR_SEND = 6,
    GALAY_ETCD_ERROR_RECV = 7,
    GALAY_ETCD_ERROR_HTTP = 8,
    GALAY_ETCD_ERROR_SERVER = 9,
    GALAY_ETCD_ERROR_PARSE = 10,
    GALAY_ETCD_ERROR_INTERNAL = 11,
    GALAY_ETCD_ERROR_UNKNOWN = 12
} galay_etcd_error_code_t;

GALAY_C_API const char* galay_etcd_error_string(galay_etcd_error_code_t code);
GALAY_C_API galay_status_t galay_etcd_error_status(galay_etcd_error_code_t code);

GALAY_C_API galay_status_t galay_etcd_config_builder_create(galay_etcd_config_builder_t** out);
GALAY_C_API void galay_etcd_config_builder_destroy(galay_etcd_config_builder_t* builder);
GALAY_C_API galay_status_t galay_etcd_config_builder_set_endpoint(galay_etcd_config_builder_t* builder,
                                                                  const char* endpoint);
GALAY_C_API galay_status_t galay_etcd_config_builder_set_api_prefix(galay_etcd_config_builder_t* builder,
                                                                    const char* api_prefix);
GALAY_C_API galay_status_t galay_etcd_config_builder_set_request_timeout_ms(galay_etcd_config_builder_t* builder,
                                                                            int64_t timeout_ms);
GALAY_C_API galay_status_t galay_etcd_config_builder_set_buffer_size(galay_etcd_config_builder_t* builder,
                                                                     size_t buffer_size);
GALAY_C_API galay_status_t galay_etcd_config_builder_set_keepalive(galay_etcd_config_builder_t* builder,
                                                                   galay_bool_t enabled);

GALAY_C_API galay_status_t galay_etcd_client_create(const galay_etcd_config_builder_t* builder,
                                                    galay_etcd_client_t** out);
GALAY_C_API void galay_etcd_client_destroy(galay_etcd_client_t* client);
GALAY_C_API galay_status_t galay_etcd_client_connect(galay_etcd_client_t* client,
                                                     galay_etcd_error_code_t* error_code);
GALAY_C_API galay_status_t galay_etcd_client_close(galay_etcd_client_t* client,
                                                   galay_etcd_error_code_t* error_code);
GALAY_C_API galay_bool_t galay_etcd_client_connected(const galay_etcd_client_t* client);
GALAY_C_API galay_status_t galay_etcd_client_put(galay_etcd_client_t* client,
                                                 const char* key,
                                                 const void* value,
                                                 size_t value_len,
                                                 galay_etcd_error_code_t* error_code);
GALAY_C_API galay_status_t galay_etcd_client_get(galay_etcd_client_t* client,
                                                 const char* key,
                                                 galay_bool_t prefix,
                                                 int64_t limit,
                                                 galay_etcd_get_result_t** out,
                                                 galay_etcd_error_code_t* error_code);
GALAY_C_API galay_status_t galay_etcd_client_delete(galay_etcd_client_t* client,
                                                    const char* key,
                                                    galay_bool_t prefix,
                                                    int64_t* deleted_count,
                                                    galay_etcd_error_code_t* error_code);

GALAY_C_API galay_status_t galay_etcd_get_result_create_empty(galay_etcd_get_result_t** out);
GALAY_C_API void galay_etcd_get_result_destroy(galay_etcd_get_result_t* result);
GALAY_C_API galay_status_t galay_etcd_get_result_count(const galay_etcd_get_result_t* result,
                                                       size_t* count);
GALAY_C_API galay_status_t galay_etcd_get_result_item(const galay_etcd_get_result_t* result,
                                                      size_t index,
                                                      const char** key,
                                                      size_t* key_len,
                                                      const char** value,
                                                      size_t* value_len);

GALAY_C_END_DECLS

#endif /* GALAY_C_ETCD_ETCD_H */
