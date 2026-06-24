#ifndef GALAY_KERNEL_TCP_SOCKET_C_H
#define GALAY_KERNEL_TCP_SOCKET_C_H

#include "../common-c/host.h"
#include "../core-c/runtime_c.h"
#include <stddef.h>

/**
 * @file tcp_socket_c.h
 * @brief Galay kernel TCP socket 的 C ABI 封装。
 *
 * @details 该头文件只暴露 C 可见的轻量句柄和状态码，实际 socket 生命周期
 * 由实现文件中的 C++ galay::async::TcpSocket 承载。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TCP socket C ABI 操作结果码。
 */
typedef enum C_TcpSocketResultCode {
    C_TcpSocketSuccess,                ///< 操作成功。
    C_TcpSocketParameterInvalid,       ///< 参数错误。
    C_TcpSocketMemoryAllocFailed,      ///< 内存或底层 socket 资源创建失败。
    C_TcpSocketIOFailed,               ///< 底层 IO 操作失败。
    C_TcpSocketOperationInvalid,       ///< 当前 socket 状态不允许执行该操作。
    C_TcpSocketRuntimeNotRunning,      ///< runtime 未启动。
    C_TcpSocketRuntimeSpawnFailed,     ///< runtime 提交任务失败。
} C_TcpSocketResultCode;

/**
 * @brief TCP socket C 句柄。
 *
 * @note socket 指向内部 C++ TcpSocket 对象，调用方不能解引用或直接释放。
 */
typedef struct galay_kernel_tcp_socket {
    void* socket;           ///< 内部 TcpSocket 对象指针。
} galay_kernel_tcp_socket_t;

/**
 * @brief TCP accept 回调结果。
 *
 * @note 成功时 socket 字段持有 accepted socket，调用方负责 destroy。
 */
typedef struct galay_kernel_tcp_accept_result {
    C_TcpSocketResultCode code;             ///< accept 结果码。
    C_Host peer;                            ///< 对端地址信息。
    galay_kernel_tcp_socket_t socket;       ///< accepted TCP socket 句柄
} galay_kernel_tcp_accept_result_t;

/**
 * @brief TCP recv 回调结果。
 */
typedef struct galay_kernel_tcp_recv_result {
    C_TcpSocketResultCode code;             ///< recv 结果码。
    char* buffer;                           ///< 调用 recv 时传入的接收缓冲区。
    size_t length;                          ///< 调用 recv 时请求的缓冲区长度。
    size_t bytes;                           ///< 成功读取的字节数。
} galay_kernel_tcp_recv_result_t;

/**
 * @brief TCP send 回调结果。
 */
typedef struct galay_kernel_tcp_send_result {
    C_TcpSocketResultCode code;             ///< send 结果码。
    const char* buffer;                     ///< 调用 send 时传入的发送缓冲区。
    size_t length;                          ///< 调用 send 时请求发送的字节数。
    size_t bytes;                           ///< 成功写入的字节数。
} galay_kernel_tcp_send_result_t;

/**
 * @brief TCP accept 完成回调。
 *
 * @param result accept 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_tcp_socket_accept 时传入的用户上下文。
 */
typedef void (*galay_kernel_tcp_accept_callback_t)(
    galay_kernel_tcp_accept_result_t* result,
    void* ctx);

/**
 * @brief TCP accept loop 回调。
 *
 * @param result accept 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_tcp_socket_accept_loop 时传入的用户上下文。
 * @return 返回 0 继续下一轮 accept；返回非 0 在本次回调返回后停止 loop。
 */
typedef int (*galay_kernel_tcp_accept_loop_callback_t)(
    galay_kernel_tcp_accept_result_t* result,
    void* ctx);

/**
 * @brief TCP connect 完成回调。
 *
 * @param code connect 完成结果码；连接成功为 C_TcpSocketSuccess，连接失败为 C_TcpSocketIOFailed。
 * @param ctx 调用 galay_kernel_tcp_socket_connect 时传入的用户上下文。
 */
typedef void (*galay_kernel_tcp_connect_callback_t)(
    C_TcpSocketResultCode code,
    void* ctx);

/**
 * @brief TCP recv 完成回调。
 *
 * @param result recv 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_tcp_socket_recv 时传入的用户上下文。
 */
typedef void (*galay_kernel_tcp_recv_callback_t)(
    galay_kernel_tcp_recv_result_t* result,
    void* ctx);

/**
 * @brief TCP recv loop 回调。
 *
 * @param result recv 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_tcp_socket_recv_loop 时传入的用户上下文。
 * @return 返回 0 继续下一轮 recv；返回非 0 在本次回调返回后停止 loop。
 */
typedef int (*galay_kernel_tcp_recv_loop_callback_t)(
    galay_kernel_tcp_recv_result_t* result,
    void* ctx);

/**
 * @brief TCP send 完成回调。
 *
 * @param result send 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_tcp_socket_send 时传入的用户上下文。
 */
typedef void (*galay_kernel_tcp_send_callback_t)(
    galay_kernel_tcp_send_result_t* result,
    void* ctx);

/**
 * @brief TCP send loop 回调。
 *
 * @param result send 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_tcp_socket_send_loop 时传入的用户上下文。
 * @return 返回 0 继续下一轮 send；返回非 0 在本次回调返回后停止 loop。
 */
typedef int (*galay_kernel_tcp_send_loop_callback_t)(
    galay_kernel_tcp_send_result_t* result,
    void* ctx);

/**
 * @brief TCP close 完成回调。
 *
 * @param code close 完成结果码；关闭成功为 C_TcpSocketSuccess，关闭失败为 C_TcpSocketIOFailed。
 * @param ctx 调用 galay_kernel_tcp_socket_close 时传入的用户上下文。
 */
typedef void (*galay_kernel_tcp_close_callback_t)(
    C_TcpSocketResultCode code,
    void* ctx);

/**
 * @brief 将 TCP socket 结果码转换为可读错误信息。
 *
 * @param code C_TcpSocketResultCode 结果码。
 * @return 指向静态错误字符串的指针，调用方不需要释放。
 */
const char* galay_kernel_tcp_socket_get_error(C_TcpSocketResultCode code);

/**
 * @brief 创建 TCP socket。
 *
 * @param c_socket 输出 socket 句柄；成功时其 socket 字段指向内部 TcpSocket。
 * @param type IP 协议版本，只接受 C_IPTypeIPV4 或 C_IPTypeIPV6。
 * @return 成功返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；资源创建失败返回 C_TcpSocketMemoryAllocFailed。
 *
 * @note 该函数只创建底层 socket，不启动协程，也不会阻塞等待网络事件。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_create(galay_kernel_tcp_socket_t* c_socket, C_IPType type);

/**
 * @brief 销毁 TCP socket 内部资源。
 *
 * @param c_socket 由 galay_kernel_tcp_socket_create 初始化的 socket 句柄。
 * @return 成功返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid。
 *
 * @note 该函数会释放 c_socket->socket 指向的内部 TcpSocket，并将其置空。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_destroy(galay_kernel_tcp_socket_t* c_socket);

/**
 * @brief 绑定 TCP socket 到本地地址。
 *
 * @param c_socket 由 galay_kernel_tcp_socket_create 初始化的 socket 句柄。
 * @param host 本地地址配置；address 必须是合法 IPv4/IPv6 字符串。
 * @return 成功返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；系统调用失败返回 C_TcpSocketIOFailed。
 *
 * @note 该函数只执行同步 bind 设置，不启动协程。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_bind(
    galay_kernel_tcp_socket_t* c_socket,
    const C_Host* host);

/**
 * @brief 让 TCP socket 进入 listen 状态。
 *
 * @param c_socket 已绑定的 TCP socket。
 * @param backlog 等待连接队列长度，直接传给底层 listen。
 * @return 成功返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；系统调用失败返回 C_TcpSocketIOFailed。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_listen(galay_kernel_tcp_socket_t* c_socket, int backlog);

/**
 * @brief 查询 TCP socket 当前本地端点。
 *
 * @param c_socket TCP socket 句柄。
 * @param endpoint 输出本地端点信息。
 * @return 成功返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；系统调用失败返回 C_TcpSocketIOFailed。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_local_endpoint(
    const galay_kernel_tcp_socket_t* c_socket,
    C_Host* endpoint);

/**
 * @brief 异步连接到远端地址。
 *
 * @param runtime 用于驱动 connect 协程的 runtime；必须存活到 callback 完成。
 * @param c_socket 由 galay_kernel_tcp_socket_create 初始化的 socket 句柄。
 * @param host 远端地址配置；address 必须是合法 IPv4/IPv6 字符串。
 * @param callback connect 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交连接操作返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；runtime 未运行返回 C_TcpSocketRuntimeNotRunning；提交失败返回 C_TcpSocketRuntimeSpawnFailed。
 *
 * @note 该函数不会阻塞等待连接完成；最终连接结果通过 callback 上报，callback 在 runtime 调度线程上执行。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_connect(
    galay_kernel_runtime_t* c_runtime,
    galay_kernel_tcp_socket_t* c_socket,
    const C_Host* host,
    galay_kernel_tcp_connect_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步接受一个 TCP 连接。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param listener 已 bind/listen 的 TCP 监听 socket。
 * @param callback accept 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；runtime 未运行返回 C_TcpSocketRuntimeNotRunning；提交失败返回 C_TcpSocketRuntimeSpawnFailed。
 *
 * @note 回调中的 result 只在回调期间有效；成功时 result->socket 由调用方负责 destroy。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_accept(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* listener,
    galay_kernel_tcp_accept_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上循环接受 TCP 连接。
 *
 * @param runtime 已启动的 runtime；必须存活到 loop 退出。
 * @param listener 已 bind/listen 的 TCP 监听 socket；必须存活到 loop 退出。
 * @param callback 每次 accept 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交 loop 返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；runtime 未运行返回 C_TcpSocketRuntimeNotRunning；提交失败返回 C_TcpSocketRuntimeSpawnFailed。
 *
 * @note 该函数只提交一个内部循环协程；每次接受到连接都会触发 callback。
 *       成功时 result->socket 由调用方负责 destroy；accept 出错时会回调一次错误结果并退出 loop。
 *       listener 必须存活到错误回调或 callback 返回非 0 使 loop 退出之后。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_accept_loop(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* listener,
    galay_kernel_tcp_accept_loop_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步接收 TCP 数据。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_socket TCP socket 句柄；必须存活到 callback 完成。
 * @param buffer 接收缓冲区；必须存活到 callback 完成。
 * @param length 接收缓冲区长度，必须大于 0。
 * @param callback recv 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；runtime 未运行返回 C_TcpSocketRuntimeNotRunning；提交失败返回 C_TcpSocketRuntimeSpawnFailed。
 *
 * @note 该函数不会阻塞等待数据；最终读取字节数通过 callback 上报。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_recv(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    char* buffer,
    size_t length,
    galay_kernel_tcp_recv_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上循环接收 TCP 数据。
 *
 * @param runtime 已启动的 runtime；必须存活到 loop 退出。
 * @param c_socket TCP socket 句柄；必须存活到 loop 退出。
 * @param buffer 接收缓冲区；必须存活到 loop 退出。
 * @param length 接收缓冲区长度，必须大于 0。
 * @param callback 每次 recv 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交 loop 返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；runtime 未运行返回 C_TcpSocketRuntimeNotRunning；提交失败返回 C_TcpSocketRuntimeSpawnFailed。
 *
 * @note 该函数只提交一个内部循环协程；每次读取完成都会触发 callback。
 *       buffer 会被下一轮 recv 复用，callback 中需要立即处理或复制数据。
 *       recv 出错或读取到 0 字节时会回调一次终态结果并退出 loop。
 *       callback 返回非 0 会在当前回调返回后停止 loop。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_recv_loop(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    char* buffer,
    size_t length,
    galay_kernel_tcp_recv_loop_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步发送 TCP 数据。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_socket TCP socket 句柄；必须存活到 callback 完成。
 * @param buffer 发送缓冲区；必须存活到 callback 完成。
 * @param length 发送字节数，必须大于 0。
 * @param callback send 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；runtime 未运行返回 C_TcpSocketRuntimeNotRunning；提交失败返回 C_TcpSocketRuntimeSpawnFailed。
 *
 * @note 该函数不会阻塞等待发送完成；最终写入字节数通过 callback 上报。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_send(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    const char* buffer,
    size_t length,
    galay_kernel_tcp_send_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上循环发送同一段 TCP 数据。
 *
 * @param runtime 已启动的 runtime；必须存活到 loop 退出。
 * @param c_socket TCP socket 句柄；必须存活到 loop 退出。
 * @param buffer 发送缓冲区；必须存活到 loop 退出。
 * @param length 每轮发送字节数，必须大于 0。
 * @param callback 每次 send 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交 loop 返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；runtime 未运行返回 C_TcpSocketRuntimeNotRunning；提交失败返回 C_TcpSocketRuntimeSpawnFailed。
 *
 * @note 该函数只提交一个内部循环协程；每轮都会发送同一个 buffer 并触发 callback。
 *       send 出错或写入 0 字节时会回调一次终态结果并退出 loop。
 *       callback 返回非 0 会在当前回调返回后停止 loop。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_send_loop(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    const char* buffer,
    size_t length,
    galay_kernel_tcp_send_loop_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步关闭 TCP socket。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_socket TCP socket 句柄；关闭后仍需调用 destroy 释放句柄对象。
 * @param callback close 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_TcpSocketSuccess；参数无效返回 C_TcpSocketParameterInvalid；runtime 未运行返回 C_TcpSocketRuntimeNotRunning；提交失败返回 C_TcpSocketRuntimeSpawnFailed。
 *
 * @note 该函数只关闭底层 socket，不释放 c_socket 句柄对象；最终关闭结果通过 callback 上报。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_close(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    galay_kernel_tcp_close_callback_t callback,
    void* ctx);

#ifdef __cplusplus
}
#endif

#endif
