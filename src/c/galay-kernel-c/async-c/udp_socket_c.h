#ifndef GALAY_KERNEL_UDP_SOCKET_C_H
#define GALAY_KERNEL_UDP_SOCKET_C_H

#include "../common-c/host.h"
#include "../coro-c/coro_result_c.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @file udp_socket_c.h
 * @brief Galay kernel UDP socket 的 direct C coroutine ABI。
 *
 * @details 该头文件保留 UDP socket 生命周期同步操作；会挂起的 I/O 操作必须在
 * `galay_coro_spawn` 创建的 C coroutine 内调用，并通过 `C_IOResult` 直接返回。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum C_UdpSocketResultCode {
    C_UdpSocketSuccess,
    C_UdpSocketParameterInvalid,
    C_UdpSocketMemoryAllocFailed,
    C_UdpSocketIOFailed,
    C_UdpSocketOperationInvalid,
} C_UdpSocketResultCode;

typedef struct galay_kernel_udp_socket {
    void* socket;
} galay_kernel_udp_socket_t;

const char* galay_kernel_udp_socket_get_error(C_UdpSocketResultCode code);

C_UdpSocketResultCode galay_kernel_udp_socket_create(
    galay_kernel_udp_socket_t* c_socket,
    C_IPType type);

C_UdpSocketResultCode galay_kernel_udp_socket_destroy(galay_kernel_udp_socket_t* c_socket);

C_UdpSocketResultCode galay_kernel_udp_socket_bind(
    galay_kernel_udp_socket_t* c_socket,
    const C_Host* host);

C_UdpSocketResultCode galay_kernel_udp_socket_local_endpoint(
    const galay_kernel_udp_socket_t* c_socket,
    C_Host* endpoint);

C_IOResult galay_kernel_udp_socket_recvfrom(
    galay_kernel_udp_socket_t* socket,
    char* buffer,
    size_t length,
    C_Host* from,
    int64_t timeout_ms);

C_IOResult galay_kernel_udp_socket_sendto(
    galay_kernel_udp_socket_t* socket,
    const char* buffer,
    size_t length,
    const C_Host* to,
    int64_t timeout_ms);

C_IOResult galay_kernel_udp_socket_close(
    galay_kernel_udp_socket_t* socket,
    int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
