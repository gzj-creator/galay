#ifndef GALAY_C_SSL_SSL_H
#define GALAY_C_SSL_SSL_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/common-c/host.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_ssl_method_t {
    GALAY_SSL_METHOD_TLS_CLIENT = 0,
    GALAY_SSL_METHOD_TLS_SERVER = 1
} galay_ssl_method_t;

typedef enum galay_ssl_verify_mode_t {
    GALAY_SSL_VERIFY_NONE = 0,
    GALAY_SSL_VERIFY_PEER = 1
} galay_ssl_verify_mode_t;

typedef enum galay_ssl_session_cache_mode_t {
    GALAY_SSL_SESSION_CACHE_OFF = 0,
    GALAY_SSL_SESSION_CACHE_CLIENT = 1,
    GALAY_SSL_SESSION_CACHE_SERVER = 2,
    GALAY_SSL_SESSION_CACHE_BOTH = 3
} galay_ssl_session_cache_mode_t;

typedef struct galay_ssl_context_t galay_ssl_context_t;
typedef struct galay_ssl_socket_t galay_ssl_socket_t;

const char* galay_ssl_get_error(galay_status_t status);
galay_status_t galay_ssl_context_create(galay_ssl_method_t method, galay_ssl_context_t** out);
void galay_ssl_context_destroy(galay_ssl_context_t* context);
galay_status_t galay_ssl_context_load_certificate(galay_ssl_context_t* context, const char* path);
galay_status_t galay_ssl_context_load_private_key(galay_ssl_context_t* context, const char* path);
galay_status_t galay_ssl_context_load_ca(galay_ssl_context_t* context, const char* path);
galay_status_t galay_ssl_context_set_verify_mode(galay_ssl_context_t* context,
                                                 galay_ssl_verify_mode_t mode);
/**
 * @brief 设置客户端 ALPN offer 列表。
 * @note protocols/count 仅在调用期间借用；每个协议必须非空且长度不超过 255 字节。
 */
galay_status_t galay_ssl_context_set_alpn_protocols(galay_ssl_context_t* context,
                                                    const char* const* protocols,
                                                    size_t count);
/**
 * @brief 设置服务端 ALPN 选择优先级。
 * @note protocols/count 仅在调用期间借用；列表顺序表示服务端选择优先级。
 */
galay_status_t galay_ssl_context_set_alpn_select_protocols(galay_ssl_context_t* context,
                                                           const char* const* protocols,
                                                           size_t count);
/**
 * @brief 设置 OpenSSL session cache 模式。
 * @return 参数无效返回 GALAY_INVALID_ARGUMENT，成功返回 GALAY_OK。
 */
galay_status_t galay_ssl_context_set_session_cache_mode(galay_ssl_context_t* context,
                                                        galay_ssl_session_cache_mode_t mode);
/**
 * @brief 设置 session 超时时间，单位为秒。
 * @note timeout_seconds 必须大于等于 0。
 */
galay_status_t galay_ssl_context_set_session_timeout(galay_ssl_context_t* context,
                                                     long timeout_seconds);
/**
 * @brief 禁用 context 的 session cache。
 */
galay_status_t galay_ssl_context_disable_session_cache(galay_ssl_context_t* context);
/**
 * @brief 禁用 context 的 TLS session ticket。
 */
galay_status_t galay_ssl_context_disable_session_tickets(galay_ssl_context_t* context);

galay_status_t galay_ssl_socket_create(galay_ssl_context_t* context, C_IPType type,
                                       galay_ssl_socket_t** out);
void galay_ssl_socket_destroy(galay_ssl_socket_t* socket);
galay_status_t galay_ssl_socket_bind(galay_ssl_socket_t* socket, const C_Host* host);
galay_status_t galay_ssl_socket_listen(galay_ssl_socket_t* socket, int backlog);
galay_status_t galay_ssl_socket_local_endpoint(const galay_ssl_socket_t* socket, C_Host* out);
galay_status_t galay_ssl_socket_set_hostname(galay_ssl_socket_t* socket, const char* hostname);
C_IOResult galay_ssl_socket_accept(galay_ssl_socket_t* listener, galay_ssl_socket_t** out,
                                   C_Host* out_peer, int64_t timeout_ms);
C_IOResult galay_ssl_socket_connect(galay_ssl_socket_t* socket, const C_Host* host,
                                    int64_t timeout_ms);
C_IOResult galay_ssl_socket_handshake(galay_ssl_socket_t* socket, int64_t timeout_ms);
C_IOResult galay_ssl_socket_recv(galay_ssl_socket_t* socket, char* buffer, size_t length,
                                 int64_t timeout_ms);
C_IOResult galay_ssl_socket_send(galay_ssl_socket_t* socket, const char* buffer, size_t length,
                                 int64_t timeout_ms);
C_IOResult galay_ssl_socket_shutdown(galay_ssl_socket_t* socket, int64_t timeout_ms);
C_IOResult galay_ssl_socket_close(galay_ssl_socket_t* socket, int64_t timeout_ms);
galay_status_t galay_ssl_socket_get_protocol(const galay_ssl_socket_t* socket, char* out,
                                             size_t out_len, size_t* written);
galay_status_t galay_ssl_socket_get_cipher(const galay_ssl_socket_t* socket, char* out,
                                           size_t out_len, size_t* written);
/**
 * @brief 获取握手后协商出的 ALPN 协议。
 * @note out 由调用方拥有；写入内容不追加 NUL，实际字节数通过 written 返回。
 */
galay_status_t galay_ssl_socket_get_negotiated_alpn(const galay_ssl_socket_t* socket, char* out,
                                                    size_t out_len, size_t* written);

#ifdef __cplusplus
}
#endif

#endif
