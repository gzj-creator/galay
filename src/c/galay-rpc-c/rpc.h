#ifndef GALAY_C_RPC_RPC_H
#define GALAY_C_RPC_RPC_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/common-c/host.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GALAY_RPC_HEADER_SIZE 16u

typedef enum galay_rpc_call_mode_t {
    GALAY_RPC_CALL_UNARY = 1,
    GALAY_RPC_CALL_CLIENT_STREAMING = 2,
    GALAY_RPC_CALL_SERVER_STREAMING = 3,
    GALAY_RPC_CALL_BIDI_STREAMING = 4
} galay_rpc_call_mode_t;

typedef enum galay_rpc_error_code_t {
    GALAY_RPC_ERROR_OK = 0,
    GALAY_RPC_ERROR_UNKNOWN_ERROR = 1,
    GALAY_RPC_ERROR_SERVICE_NOT_FOUND = 2,
    GALAY_RPC_ERROR_METHOD_NOT_FOUND = 3,
    GALAY_RPC_ERROR_INVALID_REQUEST = 4,
    GALAY_RPC_ERROR_INVALID_RESPONSE = 5,
    GALAY_RPC_ERROR_REQUEST_TIMEOUT = 6,
    GALAY_RPC_ERROR_CONNECTION_CLOSED = 7,
    GALAY_RPC_ERROR_SERIALIZATION_ERROR = 8,
    GALAY_RPC_ERROR_DESERIALIZATION_ERROR = 9,
    GALAY_RPC_ERROR_INTERNAL_ERROR = 10,
    GALAY_RPC_ERROR_CANCELLED = 11,
    GALAY_RPC_ERROR_DEADLINE_EXCEEDED = 12,
    GALAY_RPC_ERROR_RESOURCE_EXHAUSTED = 13,
    GALAY_RPC_ERROR_RATE_LIMITED = 14,
    GALAY_RPC_ERROR_CIRCUIT_OPEN = 15,
    GALAY_RPC_ERROR_UNAUTHENTICATED = 16,
    GALAY_RPC_ERROR_PERMISSION_DENIED = 17,
    GALAY_RPC_ERROR_UNAVAILABLE = 18
} galay_rpc_error_code_t;

typedef struct galay_rpc_request_t {
    uint32_t request_id;
    galay_rpc_call_mode_t call_mode;
    galay_bool_t end_of_stream;
    const char* service;
    size_t service_len;
    const char* method;
    size_t method_len;
    const void* payload;
    size_t payload_len;
} galay_rpc_request_t;

typedef struct galay_rpc_response_t {
    uint32_t request_id;
    galay_rpc_call_mode_t call_mode;
    galay_bool_t end_of_stream;
    galay_rpc_error_code_t error_code;
    const void* payload;
    size_t payload_len;
} galay_rpc_response_t;

typedef struct galay_rpc_client_t galay_rpc_client_t;
typedef struct galay_rpc_server_t galay_rpc_server_t;
typedef struct galay_rpc_service_t galay_rpc_service_t;
typedef struct galay_rpc_stream_t galay_rpc_stream_t;
typedef struct galay_rpc_cancellation_source_t galay_rpc_cancellation_source_t;
typedef struct galay_rpc_pool_t galay_rpc_pool_t;
typedef struct galay_rpc_pool_lease_t galay_rpc_pool_lease_t;

typedef struct galay_rpc_client_config_t {
    const char* host;             ///< 远端地址；NULL 或空字符串时默认 127.0.0.1。
    uint16_t port;                ///< 远端端口；0 表示无效。
    int64_t connect_timeout_ms;   ///< connect 默认超时；负数表示无限等待。
} galay_rpc_client_config_t;

typedef struct galay_rpc_server_config_t {
    const char* host;             ///< 监听地址；NULL 或空字符串时默认 127.0.0.1。
    uint16_t port;                ///< 监听端口；0 表示由系统分配。
    int backlog;                  ///< listen backlog；<=0 时使用默认值 128。
} galay_rpc_server_config_t;

typedef struct galay_rpc_call_options_t {
    int64_t timeout_ms;                                      ///< 每次 socket I/O 超时。
    const galay_rpc_cancellation_source_t* cancellation;     ///< 可选取消源；调用期间必须有效。
} galay_rpc_call_options_t;

typedef struct galay_rpc_response_buffer_t {
    uint32_t request_id;
    galay_rpc_call_mode_t call_mode;
    galay_bool_t end_of_stream;
    galay_rpc_error_code_t error_code;
    void* payload;       ///< 成功时由 C ABI 分配；调用方用 galay_rpc_response_buffer_destroy 释放。
    size_t payload_len;
} galay_rpc_response_buffer_t;

typedef struct galay_rpc_pool_config_t {
    size_t min_connections_per_endpoint;
    size_t max_connections_per_endpoint;
} galay_rpc_pool_config_t;

/**
 * @brief C RPC 方法处理器。
 * @details 回调在 server coroutine 中同步执行；request 的指针和 payload 仅在回调期间有效。
 * response 的 payload 可借用用户内存，server 会在回调返回后、发送完成前同步读取。
 */
typedef galay_rpc_error_code_t (*galay_rpc_method_handler_fn)(
    const galay_rpc_request_t* request,
    galay_rpc_response_t* response,
    void* user_data);

const char* galay_rpc_error_string(galay_rpc_error_code_t code);
galay_status_t galay_rpc_error_to_status(galay_rpc_error_code_t code);
galay_bool_t galay_rpc_name_is_valid(const char* name, size_t name_len);
galay_status_t galay_rpc_request_encoded_size(const galay_rpc_request_t* request, size_t* size);
galay_status_t galay_rpc_response_encoded_size(const galay_rpc_response_t* response, size_t* size);
galay_status_t galay_rpc_encode_request(const galay_rpc_request_t* request, uint8_t* out,
                                        size_t out_len, size_t* written);
galay_status_t galay_rpc_encode_response(const galay_rpc_response_t* response, uint8_t* out,
                                         size_t out_len, size_t* written);
galay_status_t galay_rpc_decode_request(const uint8_t* data, size_t data_len,
                                        galay_rpc_request_t* out, size_t* consumed,
                                        galay_rpc_error_code_t* rpc_error);
galay_status_t galay_rpc_decode_response(const uint8_t* data, size_t data_len,
                                         galay_rpc_response_t* out, size_t* consumed,
                                         galay_rpc_error_code_t* rpc_error);

galay_rpc_client_config_t galay_rpc_client_config_default(void);
galay_rpc_server_config_t galay_rpc_server_config_default(void);
galay_rpc_call_options_t galay_rpc_call_options_default(void);
galay_rpc_pool_config_t galay_rpc_pool_config_default(void);

galay_status_t galay_rpc_client_create(const galay_rpc_client_config_t* config,
                                       galay_rpc_client_t** out);
void galay_rpc_client_destroy(galay_rpc_client_t* client);
C_IOResult galay_rpc_client_connect(galay_rpc_client_t* client, int64_t timeout_ms);
C_IOResult galay_rpc_client_close(galay_rpc_client_t* client, int64_t timeout_ms);
C_IOResult galay_rpc_client_heartbeat(galay_rpc_client_t* client, int64_t timeout_ms);
C_IOResult galay_rpc_client_call(galay_rpc_client_t* client,
                                 const char* service,
                                 size_t service_len,
                                 const char* method,
                                 size_t method_len,
                                 const void* payload,
                                 size_t payload_len,
                                 int64_t timeout_ms,
                                 galay_rpc_response_buffer_t* out_response);
C_IOResult galay_rpc_client_call_with_options(galay_rpc_client_t* client,
                                              const char* service,
                                              size_t service_len,
                                              const char* method,
                                              size_t method_len,
                                              const void* payload,
                                              size_t payload_len,
                                              const galay_rpc_call_options_t* options,
                                              galay_rpc_response_buffer_t* out_response);

galay_status_t galay_rpc_server_create(const galay_rpc_server_config_t* config,
                                       galay_rpc_server_t** out);
void galay_rpc_server_destroy(galay_rpc_server_t* server);
galay_status_t galay_rpc_server_listen(galay_rpc_server_t* server);
galay_status_t galay_rpc_server_local_endpoint(galay_rpc_server_t* server, C_Host* out);
galay_status_t galay_rpc_server_register_service(galay_rpc_server_t* server,
                                                 galay_rpc_service_t* service);
C_IOResult galay_rpc_server_serve_one(galay_rpc_server_t* server, int64_t timeout_ms);

galay_status_t galay_rpc_service_create(const char* name,
                                        size_t name_len,
                                        galay_rpc_service_t** out);
void galay_rpc_service_destroy(galay_rpc_service_t* service);
galay_status_t galay_rpc_service_register_unary(galay_rpc_service_t* service,
                                                const char* method,
                                                size_t method_len,
                                                galay_rpc_method_handler_fn handler,
                                                void* user_data);
galay_status_t galay_rpc_service_register_streaming(galay_rpc_service_t* service,
                                                    const char* method,
                                                    size_t method_len,
                                                    galay_rpc_call_mode_t mode,
                                                    galay_rpc_method_handler_fn handler,
                                                    void* user_data);

C_IOResult galay_rpc_client_stream_open(galay_rpc_client_t* client,
                                        const char* service,
                                        size_t service_len,
                                        const char* method,
                                        size_t method_len,
                                        galay_rpc_call_mode_t mode,
                                        galay_rpc_stream_t** out_stream);
C_IOResult galay_rpc_stream_write(galay_rpc_stream_t* stream,
                                  const void* payload,
                                  size_t payload_len,
                                  galay_bool_t end_of_stream,
                                  int64_t timeout_ms);
C_IOResult galay_rpc_stream_read(galay_rpc_stream_t* stream,
                                 int64_t timeout_ms,
                                 galay_rpc_response_buffer_t* out_response);
C_IOResult galay_rpc_stream_close(galay_rpc_stream_t* stream, int64_t timeout_ms);
void galay_rpc_stream_destroy(galay_rpc_stream_t* stream);

galay_status_t galay_rpc_cancellation_source_create(galay_rpc_cancellation_source_t** out);
void galay_rpc_cancellation_source_cancel(galay_rpc_cancellation_source_t* source);
void galay_rpc_cancellation_source_destroy(galay_rpc_cancellation_source_t* source);

void galay_rpc_response_buffer_destroy(galay_rpc_response_buffer_t* response);

galay_status_t galay_rpc_pool_create(const galay_rpc_pool_config_t* config,
                                     galay_rpc_pool_t** out);
void galay_rpc_pool_destroy(galay_rpc_pool_t* pool);
galay_status_t galay_rpc_pool_ensure_endpoint(galay_rpc_pool_t* pool,
                                              const char* host,
                                              uint16_t port);
galay_status_t galay_rpc_pool_acquire(galay_rpc_pool_t* pool,
                                      const char* host,
                                      uint16_t port,
                                      galay_rpc_pool_lease_t** out_lease);
galay_status_t galay_rpc_pool_release(galay_rpc_pool_t* pool,
                                      galay_rpc_pool_lease_t* lease,
                                      galay_bool_t broken);
galay_status_t galay_rpc_pool_shutdown(galay_rpc_pool_t* pool);
galay_status_t galay_rpc_pool_available_count(const galay_rpc_pool_t* pool,
                                              const char* host,
                                              uint16_t port,
                                              size_t* out_count);
galay_status_t galay_rpc_pool_in_use_count(const galay_rpc_pool_t* pool,
                                           const char* host,
                                           uint16_t port,
                                           size_t* out_count);
galay_status_t galay_rpc_pool_lease_id(const galay_rpc_pool_lease_t* lease, uint64_t* out_id);

#ifdef __cplusplus
}
#endif

#endif
