#ifndef GALAY_C_KERNEL_ASYNC_UDP_SOCKET_H
#define GALAY_C_KERNEL_ASYNC_UDP_SOCKET_H

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
 * @brief 原生 C UDP socket。
 * @details socket 可在普通线程创建和 bind；第一次 recvfrom/sendto 必须
 *          发生在 C coroutine 中，并把 socket 固定到当前 scheduler。固定后
 *          不得跨 scheduler 使用。close 可由拥有者在无并发操作时调用。
 */
typedef struct galay_c_udp_socket {
    galay_c_io_controller_t controller;
    galay_c_io_scheduler_t* scheduler;
    int fd;
    C_IPType type;
} galay_c_udp_socket_t;

/** 创建非阻塞 UDP socket；成功后由调用方负责 close。 */
C_IOResult galay_c_udp_socket_create(galay_c_udp_socket_t* out_socket, C_IPType type);

/** 绑定本地端点；host 类型必须与 create 的类型一致。 */
C_IOResult galay_c_udp_socket_bind(galay_c_udp_socket_t* socket, const C_Host* host);

/** 查询本地端点。 */
C_IOResult galay_c_udp_socket_local_endpoint(const galay_c_udp_socket_t* socket,
                                             C_Host* out);

/** 在当前 C coroutine 中接收数据报。 */
C_IOResult galay_c_udp_socket_recvfrom(galay_c_udp_socket_t* socket,
                                       char* buffer,
                                       size_t length,
                                       C_Host* out_peer,
                                       int64_t timeout_ms);

/** 在当前 C coroutine 中发送数据报。 */
C_IOResult galay_c_udp_socket_sendto(galay_c_udp_socket_t* socket,
                                     const char* buffer,
                                     size_t length,
                                     const C_Host* peer,
                                     int64_t timeout_ms);

/** 取消挂起操作并关闭 fd；重复 close 返回成功。 */
C_IOResult galay_c_udp_socket_close(galay_c_udp_socket_t* socket);

#ifdef __cplusplus
}
#endif

#endif
