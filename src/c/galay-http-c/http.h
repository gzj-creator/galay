#ifndef GALAY_C_HTTP_HTTP_H
#define GALAY_C_HTTP_HTTP_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/common-c/host.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_http_method_t {
    GALAY_HTTP_METHOD_GET = 0,
    GALAY_HTTP_METHOD_POST = 1
} galay_http_method_t;

typedef enum galay_http_status_code_t {
    GALAY_HTTP_STATUS_OK = 200,
    GALAY_HTTP_STATUS_NO_CONTENT = 204,
    GALAY_HTTP_STATUS_BAD_REQUEST = 400,
    GALAY_HTTP_STATUS_NOT_FOUND = 404,
    GALAY_HTTP_STATUS_INTERNAL_SERVER_ERROR = 500
} galay_http_status_code_t;

typedef struct galay_http_headers_t galay_http_headers_t;
typedef struct galay_http_request_t galay_http_request_t;
typedef struct galay_http_response_t galay_http_response_t;
typedef struct galay_http_client_t galay_http_client_t;
typedef struct galay_http_server_t galay_http_server_t;
typedef struct galay_http_session_t galay_http_session_t;

/**
 * @brief C HTTP 路由回调。
 *
 * @details `request` 和 `response` 仅在回调期间有效；回调不得保存这两个指针。
 * 回调通过 `response` 填充响应并以 `galay_status_t` 显式返回失败原因。
 */
typedef galay_status_t (*galay_http_route_callback_t)(
    const galay_http_request_t* request,
    galay_http_response_t* response,
    void* user_data);

const char* galay_http_get_error(galay_status_t status);
galay_status_t galay_http_headers_create(galay_http_headers_t** out);
void galay_http_headers_destroy(galay_http_headers_t* headers);
galay_status_t galay_http_headers_add(galay_http_headers_t* headers, const char* name,
                                      const char* value);
galay_status_t galay_http_headers_find(const galay_http_headers_t* headers, const char* name,
                                       const char** value, size_t* value_len);
galay_status_t galay_http_headers_remove(galay_http_headers_t* headers, const char* name);

galay_status_t galay_http_request_create(galay_http_request_t** out);
void galay_http_request_destroy(galay_http_request_t* request);
galay_status_t galay_http_request_set_method_path(galay_http_request_t* request,
                                                  galay_http_method_t method,
                                                  const char* path);
galay_status_t galay_http_request_method(const galay_http_request_t* request,
                                         galay_http_method_t* method);
galay_status_t galay_http_request_path(const galay_http_request_t* request,
                                       const char** path, size_t* path_len);
galay_status_t galay_http_request_add_header(galay_http_request_t* request, const char* name,
                                             const char* value);
galay_status_t galay_http_request_set_body(galay_http_request_t* request, const char* body,
                                           size_t body_len);
galay_status_t galay_http_request_serialize(galay_http_request_t* request,
                                            const char** data, size_t* data_len);
galay_status_t galay_http_request_parse(galay_http_request_t* request, const char* data,
                                        size_t data_len, size_t max_header_len,
                                        size_t max_body_len, size_t* consumed);
galay_bool_t galay_http_request_is_complete(const galay_http_request_t* request);
galay_status_t galay_http_request_body(const galay_http_request_t* request, const char** body,
                                       size_t* body_len);
galay_status_t galay_http_request_find_header(const galay_http_request_t* request,
                                              const char* name, const char** value,
                                              size_t* value_len);

galay_status_t galay_http_response_create(galay_http_response_t** out);
void galay_http_response_destroy(galay_http_response_t* response);
galay_status_t galay_http_response_set_status(galay_http_response_t* response,
                                              galay_http_status_code_t status);
galay_status_t galay_http_response_status(const galay_http_response_t* response,
                                          galay_http_status_code_t* status);
galay_status_t galay_http_response_add_header(galay_http_response_t* response, const char* name,
                                              const char* value);
galay_status_t galay_http_response_set_body(galay_http_response_t* response, const char* body,
                                            size_t body_len);
galay_status_t galay_http_response_body(const galay_http_response_t* response, const char** body,
                                        size_t* body_len);
galay_status_t galay_http_response_parse(galay_http_response_t* response, const char* data,
                                         size_t data_len, size_t max_header_len,
                                         size_t max_body_len, size_t* consumed);
galay_status_t galay_http_response_serialize(galay_http_response_t* response,
                                             const char** data, size_t* data_len);

/**
 * @brief 创建/销毁 HTTP client 句柄。
 *
 * @note client 独占其 TCP socket；所有 I/O API 必须在 `galay_coro_spawn`
 * 创建的 C coroutine 内调用。destroy 会释放句柄，不会抛出异常。
 */
galay_status_t galay_http_client_create(galay_http_client_t** out);
galay_status_t galay_http_client_destroy(galay_http_client_t* client);
C_IOResult galay_http_client_connect(galay_http_client_t* client, const C_Host* endpoint,
                                     int64_t timeout_ms);
C_IOResult galay_http_client_send_request(galay_http_client_t* client,
                                          const galay_http_request_t* request,
                                          int64_t timeout_ms);
C_IOResult galay_http_client_recv_response(galay_http_client_t* client,
                                           galay_http_response_t** out_response,
                                           size_t max_header_len,
                                           size_t max_body_len,
                                           int64_t timeout_ms);
C_IOResult galay_http_client_close(galay_http_client_t* client, int64_t timeout_ms);

/**
 * @brief HTTP server 句柄与路由注册。
 *
 * @details server 独占 listener socket；`serve_one` 接受一个连接、读取一个请求、
 * 调用匹配路由并发送一个响应，然后关闭该 session。
 */
galay_status_t galay_http_server_create(galay_http_server_t** out);
galay_status_t galay_http_server_destroy(galay_http_server_t* server);
galay_status_t galay_http_server_bind(galay_http_server_t* server, const C_Host* endpoint);
galay_status_t galay_http_server_listen(galay_http_server_t* server, int backlog);
galay_status_t galay_http_server_local_endpoint(const galay_http_server_t* server,
                                                C_Host* endpoint);
galay_status_t galay_http_server_add_route(galay_http_server_t* server,
                                           galay_http_method_t method,
                                           const char* path,
                                           galay_http_route_callback_t callback,
                                           void* user_data);
C_IOResult galay_http_server_accept(galay_http_server_t* server,
                                    galay_http_session_t** out_session,
                                    C_Host* out_peer,
                                    int64_t timeout_ms);
C_IOResult galay_http_server_serve_one(galay_http_server_t* server, int64_t timeout_ms);
C_IOResult galay_http_server_stop(galay_http_server_t* server, int64_t timeout_ms);

/**
 * @brief HTTP session direct coroutine I/O。
 *
 * @details `recv_request` / `recv_response` 返回新分配的 request/response，调用方必须用
 * 对应 destroy 函数释放。`send_bytes` / `recv_bytes` 是最小 streaming surface，
 * 用于测试分片、超时、关闭和协议错误路径。
 */
galay_status_t galay_http_session_destroy(galay_http_session_t* session);
C_IOResult galay_http_session_send_request(galay_http_session_t* session,
                                           const galay_http_request_t* request,
                                           int64_t timeout_ms);
C_IOResult galay_http_session_recv_request(galay_http_session_t* session,
                                           galay_http_request_t** out_request,
                                           size_t max_header_len,
                                           size_t max_body_len,
                                           int64_t timeout_ms);
C_IOResult galay_http_session_send_response(galay_http_session_t* session,
                                            const galay_http_response_t* response,
                                            int64_t timeout_ms);
C_IOResult galay_http_session_recv_response(galay_http_session_t* session,
                                            galay_http_response_t** out_response,
                                            size_t max_header_len,
                                            size_t max_body_len,
                                            int64_t timeout_ms);
C_IOResult galay_http_session_send_bytes(galay_http_session_t* session, const char* data,
                                         size_t data_len, int64_t timeout_ms);
C_IOResult galay_http_session_recv_bytes(galay_http_session_t* session, char* data,
                                         size_t data_len, int64_t timeout_ms);
C_IOResult galay_http_session_close(galay_http_session_t* session, int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
