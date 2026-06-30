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

/**
 * @brief UDP socket 生命周期操作结果码。
 *
 * @details create/bind/local_endpoint/destroy 同步返回该枚举；recvfrom/sendto/close
 * 返回 C_IOResult。
 */
typedef enum C_UdpSocketResultCode {
    C_UdpSocketSuccess,               ///< 操作成功。
    C_UdpSocketParameterInvalid,      ///< 参数错误或句柄未初始化。
    C_UdpSocketMemoryAllocFailed,     ///< 内存或底层 socket 资源创建失败。
    C_UdpSocketIOFailed,              ///< 底层 IO 操作失败。
    C_UdpSocketOperationInvalid,      ///< 当前 socket 状态不允许执行该操作。
} C_UdpSocketResultCode;

/**
 * @brief UDP socket C 句柄。
 *
 * @note socket 指向内部 C++ UdpSocket 对象，调用方不能解引用或直接释放。生命周期
 * 由 create/destroy 管理。
 */
typedef struct galay_kernel_udp_socket {
    void* socket;      ///< 内部 UdpSocket 对象指针。
} galay_kernel_udp_socket_t;

/**
 * @brief 将 UDP 生命周期结果码转换为可读错误字符串。
 *
 * @param code UDP 生命周期结果码。
 * @return 指向静态只读字符串的指针，调用方不得释放。
 *
 * @note 该函数不分配内存、不会阻塞，线程安全。
 */
const char* galay_kernel_udp_socket_get_error(C_UdpSocketResultCode code);

/**
 * @brief 创建 UDP socket。
 *
 * @param c_socket 输出 socket 句柄；成功时写入内部 UdpSocket 指针。
 * @param type IP 地址族，必须是 C_IPTypeIPV4 或 C_IPTypeIPV6。
 * @return 成功返回 C_UdpSocketSuccess；参数非法返回
 * C_UdpSocketParameterInvalid；底层创建或内存分配失败返回
 * C_UdpSocketIOFailed/C_UdpSocketMemoryAllocFailed。
 *
 * @note 成功后调用方拥有 c_socket->socket，必须调用
 * galay_kernel_udp_socket_destroy 释放。该函数同步执行。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_create(
    galay_kernel_udp_socket_t* c_socket,
    C_IPType type);

/**
 * @brief 销毁 UDP socket 句柄。
 *
 * @param c_socket 由 create 初始化的 socket 句柄。
 * @return 成功返回 C_UdpSocketSuccess；c_socket 为 NULL 返回
 * C_UdpSocketParameterInvalid。
 *
 * @note 释放后会将 c_socket->socket 置为 NULL。调用方必须保证没有挂起的 UDP
 * direct C coroutine 操作仍可能访问该 socket。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_destroy(galay_kernel_udp_socket_t* c_socket);

/**
 * @brief 绑定 UDP socket 到本地地址。
 *
 * @param c_socket 已创建且未销毁的 UDP socket。
 * @param host 本地地址和端口；address 必须是以 '\0' 结尾的有效 IPv4/IPv6 字符串。
 * @return 成功返回 C_UdpSocketSuccess；参数非法返回 C_UdpSocketParameterInvalid；
 * 设置选项或 bind 失败返回 C_UdpSocketIOFailed/OperationInvalid。
 *
 * @note 该函数同步执行，会把 socket 设置为可复用地址和非阻塞模式。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_bind(
    galay_kernel_udp_socket_t* c_socket,
    const C_Host* host);

/**
 * @brief 查询 UDP socket 当前本地端点。
 *
 * @param c_socket 已创建的 UDP socket。
 * @param endpoint 输出端点地址，必须非 NULL。
 * @return 成功返回 C_UdpSocketSuccess；参数非法返回 C_UdpSocketParameterInvalid；
 * getsockname 或地址转换失败返回 C_UdpSocketIOFailed。
 *
 * @note endpoint 由调用方提供并拥有。该函数同步执行。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_local_endpoint(
    const galay_kernel_udp_socket_t* c_socket,
    C_Host* endpoint);

/**
 * @brief 挂起当前 C coroutine 并接收 UDP datagram。
 *
 * @param socket 已创建/绑定的 UDP socket。
 * @param buffer 调用方提供的输出缓冲区；length 为 0 时可为 NULL。
 * @param length 最多接收字节数。
 * @param from 可选输出发送端地址；不需要时传 NULL。
 * @param timeout_ms 负数无限等待，0 立即返回 C_IOResultTimeout，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk，result.bytes 为接收字节数；超时返回
 * C_IOResultTimeout；参数、状态或 coroutine 上下文无效返回 C_IOResultInvalid；
 * 底层错误返回 C_IOResultError。
 *
 * @note 必须在运行于 IO scheduler 的 C coroutine 内调用。buffer 和 from 必须在函数
 * 返回前保持有效；该 API 不拥有也不保存它们。
 */
C_IOResult galay_kernel_udp_socket_recvfrom(
    galay_kernel_udp_socket_t* socket,
    char* buffer,
    size_t length,
    C_Host* from,
    int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并发送 UDP datagram。
 *
 * @param socket 已创建的 UDP socket。
 * @param buffer 待发送数据；length 为 0 时可为 NULL。
 * @param length 待发送字节数。
 * @param to 目标地址和端口，必须为有效 IPv4/IPv6 host。
 * @param timeout_ms 负数无限等待，0 立即返回 C_IOResultTimeout，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk，result.bytes 为发送字节数；超时返回
 * C_IOResultTimeout；参数、状态或 coroutine 上下文无效返回 C_IOResultInvalid；
 * 底层错误返回 C_IOResultError。
 *
 * @note 必须在运行于 IO scheduler 的 C coroutine 内调用。buffer 和 to 必须在函数
 * 返回前保持有效。
 */
C_IOResult galay_kernel_udp_socket_sendto(
    galay_kernel_udp_socket_t* socket,
    const char* buffer,
    size_t length,
    const C_Host* to,
    int64_t timeout_ms);

/**
 * @brief 在当前 IO scheduler 上关闭 UDP socket。
 *
 * @param socket 已创建的 UDP socket。
 * @param timeout_ms 为 ABI 对称保留；当前实现只校验取值是否可转换为内部 timeout。
 * @return 成功返回 C_IOResultOk；参数、非 IO scheduler 上下文、存在非 direct C
 * coroutine 挂起操作或 controller 所属 scheduler 不匹配时返回 C_IOResultInvalid；
 * 底层 close 注册失败返回 C_IOResultError。
 *
 * @note close 会取消同一 socket 上挂起的 direct C coroutine recvfrom/sendto 操作，
 * 使其以 C_IOResultCancelled 完成。
 */
C_IOResult galay_kernel_udp_socket_close(
    galay_kernel_udp_socket_t* socket,
    int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
