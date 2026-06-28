#ifndef GALAY_KERNEL_TCP_SOCKET_C_H
#define GALAY_KERNEL_TCP_SOCKET_C_H

#include "../common-c/host.h"
#include "../coro-c/coro_result_c.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

/**
 * @file tcp_socket_c.h
 * @brief Galay kernel TCP socket 的 direct C coroutine ABI。
 *
 * @details 该头文件保留 TCP socket 生命周期同步操作；会挂起的 I/O 操作必须在
 * `galay_coro_spawn` 创建的 C coroutine 内调用，并通过 `C_IOResult` 直接返回。
 * C API 不再通过 runtime callback 或 C++ `Task<void>` 桥接。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TCP socket 生命周期操作结果码。
 */
typedef enum C_TcpSocketResultCode {
    C_TcpSocketSuccess,                ///< 操作成功。
    C_TcpSocketParameterInvalid,       ///< 参数错误。
    C_TcpSocketMemoryAllocFailed,      ///< 内存或底层 socket 资源创建失败。
    C_TcpSocketIOFailed,               ///< 底层 IO 操作失败。
    C_TcpSocketOperationInvalid,       ///< 当前 socket 状态不允许执行该操作。
} C_TcpSocketResultCode;

/**
 * @brief TCP socket C 句柄。
 *
 * @note socket 指向内部 C++ TcpSocket 对象，调用方不能解引用或直接释放。
 */
typedef struct galay_kernel_tcp_socket {
    void* socket;           ///< 内部 TcpSocket 对象指针。
} galay_kernel_tcp_socket_t;

const char* galay_kernel_tcp_socket_get_error(C_TcpSocketResultCode code);

C_TcpSocketResultCode galay_kernel_tcp_socket_create(
    galay_kernel_tcp_socket_t* c_socket,
    C_IPType type);

C_TcpSocketResultCode galay_kernel_tcp_socket_destroy(galay_kernel_tcp_socket_t* c_socket);

C_TcpSocketResultCode galay_kernel_tcp_socket_bind(
    galay_kernel_tcp_socket_t* c_socket,
    const C_Host* host);

C_TcpSocketResultCode galay_kernel_tcp_socket_listen(
    galay_kernel_tcp_socket_t* c_socket,
    int backlog);

C_TcpSocketResultCode galay_kernel_tcp_socket_local_endpoint(
    const galay_kernel_tcp_socket_t* c_socket,
    C_Host* endpoint);

/**
 * @brief 挂起当前 C coroutine 并接受一个 TCP 连接。
 * @param listener 已 bind/listen 的 TCP socket。
 * @param out_socket 成功时接收 accepted socket；调用前必须为空，调用方负责 destroy。
 * @param out_peer 可选输出 peer 地址；不需要时传 NULL。
 * @param timeout_ms 负数无限等待，0 不提交 accept 并直接返回超时，正数为毫秒超时。
 */
C_IOResult galay_kernel_tcp_socket_accept(
    galay_kernel_tcp_socket_t* listener,
    galay_kernel_tcp_socket_t* out_socket,
    C_Host* out_peer,
    int64_t timeout_ms);

C_IOResult galay_kernel_tcp_socket_connect(
    galay_kernel_tcp_socket_t* socket,
    const C_Host* host,
    int64_t timeout_ms);

C_IOResult galay_kernel_tcp_socket_recv(
    galay_kernel_tcp_socket_t* socket,
    char* buffer,
    size_t length,
    int64_t timeout_ms);

C_IOResult galay_kernel_tcp_socket_send(
    galay_kernel_tcp_socket_t* socket,
    const char* buffer,
    size_t length,
    int64_t timeout_ms);

C_IOResult galay_kernel_tcp_socket_readv(
    galay_kernel_tcp_socket_t* socket,
    const struct iovec* iovecs,
    size_t count,
    int64_t timeout_ms);

C_IOResult galay_kernel_tcp_socket_writev(
    galay_kernel_tcp_socket_t* socket,
    const struct iovec* iovecs,
    size_t count,
    int64_t timeout_ms);

C_IOResult galay_kernel_tcp_socket_sendfile(
    galay_kernel_tcp_socket_t* socket,
    int file_fd,
    int64_t offset,
    size_t count,
    int64_t timeout_ms);

C_IOResult galay_kernel_tcp_socket_close(
    galay_kernel_tcp_socket_t* socket,
    int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
