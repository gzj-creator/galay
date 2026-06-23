#ifndef GALAY_KERNEL_TCP_SOCKET_C_H
#define GALAY_KERNEL_TCP_SOCKET_C_H

#include "../common-c/host.h"
#include "../core-c/runtime_c.h"

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
    Success,                ///< 操作成功。
    ParameterInvalid,       ///< 参数错误。
    MemoryAllocFailed,      ///< 内存或底层 socket 资源创建失败。
    IOFailed,               ///< 底层 IO 操作失败。
    OperationInvalid,       ///< 当前 socket 状态不允许执行该操作。
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
 * @note 回调只在 accept 完成后触发；若 has_socket 为非 0，调用方需要通过
 * galay_kernel_tcp_accept_result_take_socket 取得 socket 所有权。
 */
typedef struct galay_kernel_tcp_accept_result {
    C_TcpSocketResultCode code;             ///< accept 结果码。
    int has_socket;                         ///< 非 0 表示 result 内持有 accepted socket。
    C_Host peer;                            ///< 对端地址信息。
    galay_kernel_tcp_socket_t* socket;      ///< accepted TCP socket 句柄，调用方通过 destroy 释放。
} galay_kernel_tcp_accept_result_t;

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
 * @param type IP 协议版本，只接受 IPV4 或 IPV6。
 * @return 成功返回 Success；参数无效返回 ParameterInvalid；资源创建失败返回 MemoryAllocFailed。
 *
 * @note 该函数只创建底层 socket，不启动协程，也不会阻塞等待网络事件。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_create(galay_kernel_tcp_socket_t* c_socket, C_IPType type);

/**
 * @brief 销毁 TCP socket 内部资源。
 *
 * @param c_socket 由 galay_kernel_tcp_socket_create 初始化的 socket 句柄。
 * @return 成功返回 Success；参数无效返回 ParameterInvalid。
 *
 * @note 该函数会释放 c_socket->socket 指向的内部 TcpSocket，并将其置空。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_destroy(galay_kernel_tcp_socket_t* c_socket);

/**
 * @brief 绑定 TCP socket 到本地地址。
 *
 * @param c_socket 由 galay_kernel_tcp_socket_create 初始化的 socket 句柄。
 * @param host 本地地址配置；address 必须是合法 IPv4/IPv6 字符串。
 * @return 成功返回 Success；参数无效返回 ParameterInvalid；系统调用失败返回 IOFailed。
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
 * @return 成功返回 Success；参数无效返回 ParameterInvalid；系统调用失败返回 IOFailed。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_listen(galay_kernel_tcp_socket_t* c_socket, int backlog);

/**
 * @brief 查询 TCP socket 当前本地端点。
 *
 * @param c_socket TCP socket 句柄。
 * @param endpoint 输出本地端点信息。
 * @return 成功返回 Success；参数无效返回 ParameterInvalid；系统调用失败返回 IOFailed。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_local_endpoint(
    const galay_kernel_tcp_socket_t* c_socket,
    C_Host* endpoint);

/**
 * @brief 异步连接到远端地址。
 *
 * @param c_socket 由 galay_kernel_tcp_socket_create 初始化的 socket 句柄。
 * @param host 远端地址配置；address 必须是合法 IPv4/IPv6 字符串。
 * @return 成功提交连接操作返回 Success；参数无效返回 ParameterInvalid；提交失败返回 IOFailed。
 *
 * @note 该接口对应底层 TcpSocket::connect，具体完成语义由实现侧 runtime/awaitable 处理。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_connect(
    galay_kernel_tcp_socket_t* c_socket,
    const C_Host* host);

/**
 * @brief 在 runtime 上启动 socket 绑定的 TCP accept callback loop。
 *
 * @param runtime 已启动或可启动的 runtime；必须存活到 stop_accept 完成之后。
 * @param listener 已 bind/listen 的 TCP 监听 socket。
 * @param callback 每次 accept 完成时调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功返回 Success；参数无效返回 ParameterInvalid；提交失败返回 IOFailed。
 *
 * @note 回调中的 result 只在回调期间有效；accepted socket 需通过
 * galay_kernel_tcp_accept_result_take_socket 转移所有权。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_accept(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* listener,
    galay_kernel_tcp_accept_callback_t callback,
    void* ctx);

/**
 * @brief 请求停止 TCP accept callback loop。
 *
 * @param listener 正在 accept 的监听 socket。
 * @return 成功返回 Success；参数无效返回 ParameterInvalid；唤醒或停止失败返回 IOFailed。
 *
 * @note 该函数只请求停止；是否阻塞等待由实现侧生命周期策略决定。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_stop_accept(galay_kernel_tcp_socket_t* listener);

/**
 * @brief 从 accept result 中取出 accepted TCP socket。
 *
 * @param result accept 回调收到的结果对象。
 * @param out_socket 输出 socket 句柄；成功后调用方负责 destroy。
 * @return 成功返回 Success；参数无效或没有 socket 返回 ParameterInvalid。
 *
 * @note 成功调用后 result 不再持有该 socket，重复调用会返回 ParameterInvalid。
 */
C_TcpSocketResultCode galay_kernel_tcp_accept_result_take_socket(
    galay_kernel_tcp_accept_result_t* result,
    galay_kernel_tcp_socket_t* out_socket);

#ifdef __cplusplus
}
#endif

#endif
