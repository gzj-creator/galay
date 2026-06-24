#ifndef GALAY_KERNEL_UDP_SOCKET_C_H
#define GALAY_KERNEL_UDP_SOCKET_C_H

#include "../common-c/host.h"
#include "../core-c/runtime_c.h"
#include <stddef.h>

/**
 * @file udp_socket_c.h
 * @brief Galay kernel UDP socket 的 C ABI 封装。
 *
 * @details 该头文件只暴露 C 可见的轻量句柄、结果结构和状态码，实际 socket
 * 生命周期由实现文件中的 C++ galay::async::UdpSocket 承载。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UDP socket C ABI 操作结果码。
 */
typedef enum C_UdpSocketResultCode {
    C_UdpSocketSuccess,                ///< 操作成功。
    C_UdpSocketParameterInvalid,       ///< 参数错误。
    C_UdpSocketMemoryAllocFailed,      ///< 内存或底层 socket 资源创建失败。
    C_UdpSocketIOFailed,               ///< 底层 IO 操作失败。
    C_UdpSocketOperationInvalid,       ///< 当前 socket 状态不允许执行该操作。
    C_UdpSocketRuntimeNotRunning,      ///< runtime 未启动。
    C_UdpSocketRuntimeSpawnFailed,     ///< runtime 提交任务失败。
} C_UdpSocketResultCode;

/**
 * @brief UDP socket C 句柄。
 *
 * @note socket 指向内部 C++ UdpSocket 对象，调用方不能解引用或直接释放。
 */
typedef struct galay_kernel_udp_socket {
    void* socket;           ///< 内部 UdpSocket 对象指针。
} galay_kernel_udp_socket_t;

/**
 * @brief UDP recvfrom 回调结果。
 */
typedef struct galay_kernel_udp_recvfrom_result {
    C_UdpSocketResultCode code;         ///< recvfrom 结果码。
    C_Host from;                        ///< 数据报来源地址。
    char* buffer;                       ///< 调用 recvfrom 时传入的接收缓冲区。
    size_t length;                      ///< 调用 recvfrom 时请求的缓冲区长度。
    size_t bytes;                       ///< 成功接收的字节数；0 表示合法空数据报。
} galay_kernel_udp_recvfrom_result_t;

/**
 * @brief UDP sendto 回调结果。
 */
typedef struct galay_kernel_udp_sendto_result {
    C_UdpSocketResultCode code;         ///< sendto 结果码。
    C_Host to;                          ///< 数据报目标地址。
    const char* buffer;                 ///< 调用 sendto 时传入的发送缓冲区。
    size_t length;                      ///< 调用 sendto 时请求发送的字节数。
    size_t bytes;                       ///< 成功发送的字节数；0 表示合法空数据报。
} galay_kernel_udp_sendto_result_t;

/**
 * @brief UDP recvfrom 完成回调。
 *
 * @param result recvfrom 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_udp_socket_recvfrom 时传入的用户上下文。
 */
typedef void (*galay_kernel_udp_recvfrom_callback_t)(
    galay_kernel_udp_recvfrom_result_t* result,
    void* ctx);

/**
 * @brief UDP recvfrom loop 回调。
 *
 * @param result recvfrom 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_udp_socket_recvfrom_loop 时传入的用户上下文。
 * @return 返回 0 继续下一轮 recvfrom；返回非 0 在本次回调返回后停止 loop。
 */
typedef int (*galay_kernel_udp_recvfrom_loop_callback_t)(
    galay_kernel_udp_recvfrom_result_t* result,
    void* ctx);

/**
 * @brief UDP sendto 完成回调。
 *
 * @param result sendto 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_udp_socket_sendto 时传入的用户上下文。
 */
typedef void (*galay_kernel_udp_sendto_callback_t)(
    galay_kernel_udp_sendto_result_t* result,
    void* ctx);

/**
 * @brief UDP sendto loop 回调。
 *
 * @param result sendto 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_udp_socket_sendto_loop 时传入的用户上下文。
 * @return 返回 0 继续下一轮 sendto；返回非 0 在本次回调返回后停止 loop。
 */
typedef int (*galay_kernel_udp_sendto_loop_callback_t)(
    galay_kernel_udp_sendto_result_t* result,
    void* ctx);

/**
 * @brief UDP close 完成回调。
 *
 * @param code close 完成结果码；关闭成功为 C_UdpSocketSuccess，关闭失败为 C_UdpSocketIOFailed。
 * @param ctx 调用 galay_kernel_udp_socket_close 时传入的用户上下文。
 */
typedef void (*galay_kernel_udp_close_callback_t)(
    C_UdpSocketResultCode code,
    void* ctx);

/**
 * @brief 将 UDP socket 结果码转换为可读错误信息。
 *
 * @param code C_UdpSocketResultCode 结果码。
 * @return 指向静态错误字符串的指针，调用方不需要释放。
 */
const char* galay_kernel_udp_socket_get_error(C_UdpSocketResultCode code);

/**
 * @brief 创建 UDP socket。
 *
 * @param c_socket 输出 socket 句柄；成功时其 socket 字段指向内部 UdpSocket。
 * @param type IP 协议版本，只接受 C_IPTypeIPV4 或 C_IPTypeIPV6。
 * @return 成功返回 C_UdpSocketSuccess；参数无效返回 C_UdpSocketParameterInvalid；
 * 资源创建失败返回 C_UdpSocketMemoryAllocFailed。
 *
 * @note 该函数只创建底层 socket，不启动协程，也不会阻塞等待网络事件。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_create(galay_kernel_udp_socket_t* c_socket, C_IPType type);

/**
 * @brief 销毁 UDP socket 内部资源。
 *
 * @param c_socket 由 galay_kernel_udp_socket_create 初始化的 socket 句柄。
 * @return 成功返回 C_UdpSocketSuccess；参数无效返回 C_UdpSocketParameterInvalid。
 *
 * @note 该函数会释放 c_socket->socket 指向的内部 UdpSocket，并将其置空。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_destroy(galay_kernel_udp_socket_t* c_socket);

/**
 * @brief 绑定 UDP socket 到本地地址。
 *
 * @param c_socket 由 galay_kernel_udp_socket_create 初始化的 socket 句柄。
 * @param host 本地地址配置；address 必须是合法 IPv4/IPv6 字符串。
 * @return 成功返回 C_UdpSocketSuccess；参数无效返回 C_UdpSocketParameterInvalid；
 * 系统调用失败返回 C_UdpSocketIOFailed。
 *
 * @note 该函数会先设置 SO_REUSEADDR 和非阻塞，再执行同步 bind。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_bind(
    galay_kernel_udp_socket_t* c_socket,
    const C_Host* host);

/**
 * @brief 查询 UDP socket 当前本地端点。
 *
 * @param c_socket UDP socket 句柄。
 * @param endpoint 输出本地端点信息。
 * @return 成功返回 C_UdpSocketSuccess；参数无效返回 C_UdpSocketParameterInvalid；
 * 系统调用失败返回 C_UdpSocketIOFailed。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_local_endpoint(
    const galay_kernel_udp_socket_t* c_socket,
    C_Host* endpoint);

/**
 * @brief 在 runtime 上异步接收一个 UDP 数据报。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_socket UDP socket 句柄；必须存活到 callback 完成。
 * @param buffer 接收缓冲区；length 非 0 时不能为空，且必须存活到 callback 完成。
 * @param length 接收缓冲区长度；允许为 0。
 * @param callback recvfrom 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_UdpSocketSuccess；参数无效返回 C_UdpSocketParameterInvalid；
 * runtime 未运行返回 C_UdpSocketRuntimeNotRunning；提交失败返回 C_UdpSocketRuntimeSpawnFailed。
 *
 * @note 该函数不会阻塞等待数据；收到 0 字节表示合法空数据报。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_recvfrom(
    galay_kernel_runtime_t* runtime,
    galay_kernel_udp_socket_t* c_socket,
    char* buffer,
    size_t length,
    galay_kernel_udp_recvfrom_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上循环接收 UDP 数据报。
 *
 * @param runtime 已启动的 runtime；必须存活到 loop 退出。
 * @param c_socket UDP socket 句柄；必须存活到 loop 退出。
 * @param buffer 接收缓冲区；length 非 0 时不能为空，且必须存活到 loop 退出。
 * @param length 接收缓冲区长度；允许为 0。
 * @param callback 每次 recvfrom 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交 loop 返回 C_UdpSocketSuccess；参数无效返回 C_UdpSocketParameterInvalid；
 * runtime 未运行返回 C_UdpSocketRuntimeNotRunning；提交失败返回 C_UdpSocketRuntimeSpawnFailed。
 *
 * @note 该函数只提交一个内部循环协程；0 字节空数据报不会使 loop 退出。
 *       recvfrom 出错会回调一次错误结果并退出；callback 返回非 0 会停止 loop。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_recvfrom_loop(
    galay_kernel_runtime_t* runtime,
    galay_kernel_udp_socket_t* c_socket,
    char* buffer,
    size_t length,
    galay_kernel_udp_recvfrom_loop_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步发送一个 UDP 数据报。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_socket UDP socket 句柄；必须存活到 callback 完成。
 * @param buffer 发送缓冲区；length 非 0 时不能为空，且必须存活到 callback 完成。
 * @param length 发送字节数；允许为 0，此时 buffer 可为 NULL。
 * @param to 目标地址配置；address 必须是合法 IPv4/IPv6 字符串。
 * @param callback sendto 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_UdpSocketSuccess；参数无效返回 C_UdpSocketParameterInvalid；
 * runtime 未运行返回 C_UdpSocketRuntimeNotRunning；提交失败返回 C_UdpSocketRuntimeSpawnFailed。
 *
 * @note 该函数不会阻塞等待发送完成；0 字节会发送合法空数据报。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_sendto(
    galay_kernel_runtime_t* runtime,
    galay_kernel_udp_socket_t* c_socket,
    const char* buffer,
    size_t length,
    const C_Host* to,
    galay_kernel_udp_sendto_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上循环发送同一段 UDP 数据报。
 *
 * @param runtime 已启动的 runtime；必须存活到 loop 退出。
 * @param c_socket UDP socket 句柄；必须存活到 loop 退出。
 * @param buffer 发送缓冲区；length 非 0 时不能为空，且必须存活到 loop 退出。
 * @param length 每轮发送字节数；允许为 0，此时 buffer 可为 NULL。
 * @param to 目标地址配置；address 必须是合法 IPv4/IPv6 字符串。
 * @param callback 每次 sendto 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交 loop 返回 C_UdpSocketSuccess；参数无效返回 C_UdpSocketParameterInvalid；
 * runtime 未运行返回 C_UdpSocketRuntimeNotRunning；提交失败返回 C_UdpSocketRuntimeSpawnFailed。
 *
 * @note 该函数只提交一个内部循环协程；sendto 出错会回调一次错误结果并退出；
 *       callback 返回非 0 会在当前回调返回后停止 loop。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_sendto_loop(
    galay_kernel_runtime_t* runtime,
    galay_kernel_udp_socket_t* c_socket,
    const char* buffer,
    size_t length,
    const C_Host* to,
    galay_kernel_udp_sendto_loop_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步关闭 UDP socket。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_socket UDP socket 句柄；关闭后仍需调用 destroy 释放句柄对象。
 * @param callback close 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_UdpSocketSuccess；参数无效返回 C_UdpSocketParameterInvalid；
 * runtime 未运行返回 C_UdpSocketRuntimeNotRunning；提交失败返回 C_UdpSocketRuntimeSpawnFailed。
 *
 * @note 该函数只关闭底层 socket，不释放 c_socket 句柄对象；最终关闭结果通过 callback 上报。
 */
C_UdpSocketResultCode galay_kernel_udp_socket_close(
    galay_kernel_runtime_t* runtime,
    galay_kernel_udp_socket_t* c_socket,
    galay_kernel_udp_close_callback_t callback,
    void* ctx);

#ifdef __cplusplus
}
#endif

#endif
