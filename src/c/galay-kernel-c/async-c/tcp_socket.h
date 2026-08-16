#ifndef GALAY_C_KERNEL_ASYNC_TCP_SOCKET_H
#define GALAY_C_KERNEL_ASYNC_TCP_SOCKET_H

#include <galay/c/galay-common-c/common/galay_c_iovec.h>
#include <galay/c/galay-kernel-c/common-c/host.h>
#include <galay/c/galay-kernel-c/core-c/io_controller.h>
#include <galay/c/galay-kernel-c/core-c/io_scheduler.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 原生 C TCP socket。
 * @details socket 可在普通线程创建、bind 和 listen；listener 的第一次 accept，以及
 *          client socket 的第一次 connect/recv/send 必须发生在 C coroutine 中，并把
 *          socket 固定到当前 scheduler。accept 输出的 client 尚未绑定，可交给其 session
 *          coroutine 的第一次 recv/send 绑定。固定后不得跨 scheduler 使用。close 可由
 *          拥有者在无并发操作时调用。
 */
typedef struct galay_c_tcp_socket {
    galay_c_io_controller_t controller;
    galay_c_io_scheduler_t* scheduler;
    int fd;
    C_IPType type;
} galay_c_tcp_socket_t;

/** 创建非阻塞 TCP socket；成功后由调用方负责 close。 */
C_IOResult galay_c_tcp_socket_create(galay_c_tcp_socket_t* out_socket, C_IPType type);

/** 绑定本地端点；host 类型必须与 create 的类型一致。 */
C_IOResult galay_c_tcp_socket_bind(galay_c_tcp_socket_t* socket, const C_Host* host);

/** 开始监听；backlog <= 0 使用系统默认值 128。 */
C_IOResult galay_c_tcp_socket_listen(galay_c_tcp_socket_t* socket, int backlog);

/** 查询本地端点。 */
C_IOResult galay_c_tcp_socket_local_endpoint(const galay_c_tcp_socket_t* socket, C_Host* out);

/**
 * @brief 启用或禁用 SO_REUSEPORT。
 * @param enabled 只接受 0 或 1；必须在 bind 前调用。
 * @return 成功返回 C_IOResultOk；平台不支持时返回 C_IOResultError/ENOTSUP。
 */
C_IOResult galay_c_tcp_socket_set_reuse_port(galay_c_tcp_socket_t* socket, int enabled);

/** 启用或禁用 TCP_NODELAY。 */
C_IOResult galay_c_tcp_socket_set_no_delay(galay_c_tcp_socket_t* socket, int enabled);

/**
 * @brief 在当前 C coroutine 中接受连接。
 * @param out_peer 可为 NULL；非 NULL 时返回对端地址。
 */
C_IOResult galay_c_tcp_socket_accept(galay_c_tcp_socket_t* listener,
                                     galay_c_tcp_socket_t* out_client,
                                     C_Host* out_peer,
                                     int64_t timeout_ms);

/** 在当前 C coroutine 中连接远端。 */
C_IOResult galay_c_tcp_socket_connect(galay_c_tcp_socket_t* socket,
                                      const C_Host* host,
                                      int64_t timeout_ms);

/** 在当前 C coroutine 中接收数据。 */
C_IOResult galay_c_tcp_socket_recv(galay_c_tcp_socket_t* socket,
                                   char* buffer,
                                   size_t length,
                                   int64_t timeout_ms);

/** 在当前 C coroutine 中发送数据；调用方必须处理短写。 */
C_IOResult galay_c_tcp_socket_send(galay_c_tcp_socket_t* socket,
                                   const char* buffer,
                                   size_t length,
                                   int64_t timeout_ms);

/** 在当前 C coroutine 中分散读取；iovecs 在函数返回前由调用方持有。 */
C_IOResult galay_c_tcp_socket_readv(galay_c_tcp_socket_t* socket,
                                    const galay_iovec_t* iovecs,
                                    size_t count,
                                    int64_t timeout_ms);

/** 在当前 C coroutine 中聚集发送；调用方必须处理短写。 */
C_IOResult galay_c_tcp_socket_writev(galay_c_tcp_socket_t* socket,
                                     const galay_iovec_t* iovecs,
                                     size_t count,
                                     int64_t timeout_ms);

/**
 * @brief 在当前 C coroutine 中从 file_fd 零拷贝发送数据。
 * @param file_fd 调用方持有的可读文件描述符，函数不接管其所有权。
 * @param offset 不修改 file_fd 当前偏移的非负文件偏移。
 * @param count 最多发送的字节数，必须大于 0；调用方必须处理短写和文件 EOF。
 */
C_IOResult galay_c_tcp_socket_sendfile(galay_c_tcp_socket_t* socket,
                                       int file_fd,
                                       int64_t offset,
                                       size_t count,
                                       int64_t timeout_ms);

/** 取消挂起操作并关闭 fd；重复 close 返回成功。 */
C_IOResult galay_c_tcp_socket_close(galay_c_tcp_socket_t* socket);

#ifdef __cplusplus
}
#endif

#endif
