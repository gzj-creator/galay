#ifndef GALAY_C_ETCD_ETCD_H
#define GALAY_C_ETCD_ETCD_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_etcd_error_code_t {
    GALAY_ETCD_ERROR_SUCCESS = 0,
    GALAY_ETCD_ERROR_INVALID_ENDPOINT = 1
} galay_etcd_error_code_t;

typedef struct galay_etcd_config_builder_t galay_etcd_config_builder_t;
typedef struct galay_etcd_client_t galay_etcd_client_t;
typedef struct galay_etcd_get_result_t galay_etcd_get_result_t;

const char* galay_etcd_error_string(galay_etcd_error_code_t code);
galay_status_t galay_etcd_error_status(galay_etcd_error_code_t code);
galay_status_t galay_etcd_config_builder_create(galay_etcd_config_builder_t** out);
void galay_etcd_config_builder_destroy(galay_etcd_config_builder_t* builder);
galay_status_t galay_etcd_config_builder_set_endpoint(galay_etcd_config_builder_t* builder,
                                                      const char* endpoint);
galay_status_t galay_etcd_client_create(const galay_etcd_config_builder_t* builder,
                                        galay_etcd_client_t** out);
void galay_etcd_client_destroy(galay_etcd_client_t* client);
galay_status_t galay_etcd_client_connect(galay_etcd_client_t* client,
                                         galay_etcd_error_code_t* code);
galay_status_t galay_etcd_client_put(galay_etcd_client_t* client, const char* key,
                                     const char* value, size_t value_len,
                                     galay_etcd_error_code_t* code);
galay_status_t galay_etcd_client_get(galay_etcd_client_t* client, const char* key,
                                     galay_bool_t prefix, int64_t limit,
                                     galay_etcd_get_result_t** result,
                                     galay_etcd_error_code_t* code);
galay_status_t galay_etcd_client_delete(galay_etcd_client_t* client, const char* key,
                                        galay_bool_t prefix, int64_t* deleted_count,
                                        galay_etcd_error_code_t* code);
galay_status_t galay_etcd_get_result_create_empty(galay_etcd_get_result_t** out);
void galay_etcd_get_result_destroy(galay_etcd_get_result_t* result);
galay_status_t galay_etcd_get_result_count(const galay_etcd_get_result_t* result, size_t* count);
galay_status_t galay_etcd_get_result_item(const galay_etcd_get_result_t* result, size_t index,
                                          const char** key, size_t* key_len,
                                          const char** value, size_t* value_len);

#ifdef __cplusplus
}
#endif

#endif
