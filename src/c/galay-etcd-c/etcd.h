#ifndef GALAY_C_ETCD_ETCD_H
#define GALAY_C_ETCD_ETCD_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_etcd_error_code_t {
    GALAY_ETCD_ERROR_SUCCESS = 0,
    GALAY_ETCD_ERROR_INVALID_ENDPOINT = 1,
    GALAY_ETCD_ERROR_INVALID_ARGUMENT = 2,
    GALAY_ETCD_ERROR_NOT_CONNECTED = 3,
    GALAY_ETCD_ERROR_IO = 4,
    GALAY_ETCD_ERROR_PROTOCOL = 5,
    GALAY_ETCD_ERROR_CANCELLED = 6
} galay_etcd_error_code_t;

typedef enum galay_etcd_endpoint_policy_t {
    GALAY_ETCD_ENDPOINT_POLICY_FIRST_HEALTHY = 0,
    GALAY_ETCD_ENDPOINT_POLICY_ROUND_ROBIN = 1,
    GALAY_ETCD_ENDPOINT_POLICY_STICKY_LEADER = 2
} galay_etcd_endpoint_policy_t;

typedef enum galay_etcd_pipeline_op_type_t {
    GALAY_ETCD_PIPELINE_PUT = 0,
    GALAY_ETCD_PIPELINE_GET = 1,
    GALAY_ETCD_PIPELINE_DELETE = 2
} galay_etcd_pipeline_op_type_t;

typedef enum galay_etcd_watch_event_type_t {
    GALAY_ETCD_WATCH_EVENT_UNKNOWN = 0,
    GALAY_ETCD_WATCH_EVENT_PUT = 1,
    GALAY_ETCD_WATCH_EVENT_DELETE = 2
} galay_etcd_watch_event_type_t;

typedef struct galay_etcd_client_stats_t {
    uint64_t requests;
    uint64_t request_failures;
    uint64_t retries;
    uint64_t endpoint_switches;
    uint64_t auth_refreshes;
    uint64_t watch_reconnects;
    uint64_t watch_compactions;
    uint64_t lease_keepalive_successes;
    uint64_t lease_keepalive_failures;
} galay_etcd_client_stats_t;

typedef struct galay_etcd_config_builder_t galay_etcd_config_builder_t;
typedef struct galay_etcd_client_t galay_etcd_client_t;
typedef struct galay_etcd_get_result_t galay_etcd_get_result_t;
typedef struct galay_etcd_pipeline_t galay_etcd_pipeline_t;
typedef struct galay_etcd_pipeline_result_t galay_etcd_pipeline_result_t;
typedef struct galay_etcd_watch_t galay_etcd_watch_t;
typedef struct galay_etcd_watch_event_t galay_etcd_watch_event_t;

const char* galay_etcd_error_string(galay_etcd_error_code_t code);
galay_status_t galay_etcd_error_status(galay_etcd_error_code_t code);
galay_status_t galay_etcd_config_builder_create(galay_etcd_config_builder_t** out);
void galay_etcd_config_builder_destroy(galay_etcd_config_builder_t* builder);
galay_status_t galay_etcd_config_builder_set_endpoint(galay_etcd_config_builder_t* builder,
                                                      const char* endpoint);
galay_status_t galay_etcd_config_builder_set_endpoint_policy(
    galay_etcd_config_builder_t* builder,
    galay_etcd_endpoint_policy_t policy);
galay_status_t galay_etcd_client_create(const galay_etcd_config_builder_t* builder,
                                        galay_etcd_client_t** out);
void galay_etcd_client_destroy(galay_etcd_client_t* client);
galay_status_t galay_etcd_client_connect(galay_etcd_client_t* client,
                                         galay_etcd_error_code_t* code);
galay_status_t galay_etcd_client_close(galay_etcd_client_t* client,
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
galay_status_t galay_etcd_client_lease_grant(galay_etcd_client_t* client,
                                             int64_t ttl_seconds,
                                             int64_t* lease_id,
                                             galay_etcd_error_code_t* code);
galay_status_t galay_etcd_client_lease_keepalive(galay_etcd_client_t* client,
                                                 int64_t lease_id,
                                                 int64_t* refreshed_lease_id,
                                                 galay_etcd_error_code_t* code);
galay_status_t galay_etcd_client_lease_revoke(galay_etcd_client_t* client,
                                              int64_t lease_id,
                                              galay_etcd_error_code_t* code);
galay_status_t galay_etcd_pipeline_create(galay_etcd_pipeline_t** out);
void galay_etcd_pipeline_destroy(galay_etcd_pipeline_t* pipeline);
galay_status_t galay_etcd_pipeline_add_put(galay_etcd_pipeline_t* pipeline,
                                           const char* key,
                                           const char* value,
                                           size_t value_len,
                                           int64_t lease_id);
galay_status_t galay_etcd_pipeline_add_get(galay_etcd_pipeline_t* pipeline,
                                           const char* key,
                                           galay_bool_t prefix,
                                           int64_t limit);
galay_status_t galay_etcd_pipeline_add_delete(galay_etcd_pipeline_t* pipeline,
                                              const char* key,
                                              galay_bool_t prefix);
galay_status_t galay_etcd_client_pipeline_execute(galay_etcd_client_t* client,
                                                  const galay_etcd_pipeline_t* pipeline,
                                                  galay_etcd_pipeline_result_t** result,
                                                  galay_etcd_error_code_t* code);
void galay_etcd_pipeline_result_destroy(galay_etcd_pipeline_result_t* result);
galay_status_t galay_etcd_pipeline_result_count(const galay_etcd_pipeline_result_t* result,
                                                size_t* count);
galay_status_t galay_etcd_pipeline_result_item_type(
    const galay_etcd_pipeline_result_t* result,
    size_t index,
    galay_etcd_pipeline_op_type_t* type);
galay_status_t galay_etcd_pipeline_result_item_get_result(
    const galay_etcd_pipeline_result_t* result,
    size_t index,
    const galay_etcd_get_result_t** get_result);
galay_status_t galay_etcd_pipeline_result_item_deleted_count(
    const galay_etcd_pipeline_result_t* result,
    size_t index,
    int64_t* deleted_count);
galay_status_t galay_etcd_watch_create(galay_etcd_client_t* client,
                                       const char* key,
                                       galay_bool_t prefix,
                                       galay_etcd_watch_t** watch,
                                       galay_etcd_error_code_t* code);
void galay_etcd_watch_destroy(galay_etcd_watch_t* watch);
galay_status_t galay_etcd_watch_next(galay_etcd_watch_t* watch,
                                     galay_etcd_watch_event_t** event,
                                     galay_etcd_error_code_t* code);
galay_status_t galay_etcd_watch_cancel(galay_etcd_watch_t* watch,
                                       galay_etcd_error_code_t* code);
void galay_etcd_watch_event_destroy(galay_etcd_watch_event_t* event);
galay_status_t galay_etcd_watch_event_watch_id(const galay_etcd_watch_event_t* event,
                                               int64_t* watch_id);
galay_status_t galay_etcd_watch_event_type(const galay_etcd_watch_event_t* event,
                                           galay_etcd_watch_event_type_t* type);
galay_status_t galay_etcd_watch_event_key_value(const galay_etcd_watch_event_t* event,
                                                const char** key,
                                                size_t* key_len,
                                                const char** value,
                                                size_t* value_len);
galay_status_t galay_etcd_client_stats(const galay_etcd_client_t* client,
                                       galay_etcd_client_stats_t* stats);

#ifdef __cplusplus
}
#endif

#endif
