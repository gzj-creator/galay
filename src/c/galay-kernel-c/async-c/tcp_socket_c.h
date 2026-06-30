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
 *
 * @details 生命周期函数同步返回该枚举；会挂起的 I/O 函数返回 C_IOResult。
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
 * @note socket 指向内部 C++ TcpSocket 对象，调用方不能解引用或直接释放。句柄
 * 本身可按值存放，但同一底层 socket 的生命周期必须由一个所有者串行管理。
 */
typedef struct galay_kernel_tcp_socket {
    void* socket;           ///< 内部 TcpSocket 对象指针。
} galay_kernel_tcp_socket_t;

/**
 * @brief 将 TCP 生命周期结果码转换为可读错误字符串。
 *
 * @param code TCP 生命周期结果码。
 * @return 指向静态只读字符串的指针，调用方不得释放。
 *
 * @note 该函数不分配内存、不会阻塞，线程安全。
 */
const char* galay_kernel_tcp_socket_get_error(C_TcpSocketResultCode code);

/**
 * @brief 创建 TCP socket。
 *
 * @param c_socket 输出 socket 句柄；成功时写入内部 TcpSocket 指针。
 * @param type IP 地址族，必须是 C_IPTypeIPV4 或 C_IPTypeIPV6。
 * @return 成功返回 C_TcpSocketSuccess；参数非法返回 C_TcpSocketParameterInvalid；
 * 底层创建或内存分配失败返回 C_TcpSocketMemoryAllocFailed。
 *
 * @note 成功后调用方拥有 c_socket->socket，必须调用
 * galay_kernel_tcp_socket_destroy 释放。该函数同步执行，不要求在 C coroutine 内调用。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_create(
    galay_kernel_tcp_socket_t* c_socket,
    C_IPType type);

/**
 * @brief 销毁 TCP socket 句柄。
 *
 * @param c_socket 由 create 或 accept 初始化的 socket 句柄。
 * @return 成功返回 C_TcpSocketSuccess；c_socket 为 NULL 返回
 * C_TcpSocketParameterInvalid。
 *
 * @note 释放后会将 c_socket->socket 置为 NULL。调用方必须先确保没有挂起的
 * accept/connect/recv/send/readv/writev/sendfile/close 仍可能访问该句柄。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_destroy(galay_kernel_tcp_socket_t* c_socket);

/**
 * @brief 绑定 TCP socket 到本地地址。
 *
 * @param c_socket 已创建且未销毁的 TCP socket。
 * @param host 本地地址和端口；address 必须是以 '\0' 结尾的有效 IPv4/IPv6 字符串。
 * @return 成功返回 C_TcpSocketSuccess；参数非法返回 C_TcpSocketParameterInvalid；
 * 设置 socket 选项或 bind 失败返回 C_TcpSocketIOFailed/OperationInvalid。
 *
 * @note 该函数同步执行，会把 socket 设置为可复用地址和非阻塞模式。调用方应在
 * listen/accept 前完成 bind。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_bind(
    galay_kernel_tcp_socket_t* c_socket,
    const C_Host* host);

/**
 * @brief 将已绑定 TCP socket 切换为监听状态。
 *
 * @param c_socket 已 bind 的 TCP socket。
 * @param backlog 传递给底层 listen 的等待队列长度。
 * @return 成功返回 C_TcpSocketSuccess；参数非法返回 C_TcpSocketParameterInvalid；
 * 底层 listen 失败返回 C_TcpSocketIOFailed/OperationInvalid。
 *
 * @note 该函数同步执行，不挂起 C coroutine。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_listen(
    galay_kernel_tcp_socket_t* c_socket,
    int backlog);

/**
 * @brief 查询 TCP socket 当前本地端点。
 *
 * @param c_socket 已创建的 TCP socket。
 * @param endpoint 输出端点地址，必须非 NULL。
 * @return 成功返回 C_TcpSocketSuccess；参数非法返回 C_TcpSocketParameterInvalid；
 * getsockname 或地址转换失败返回 C_TcpSocketIOFailed。
 *
 * @note endpoint 由调用方提供并拥有。该函数同步执行。
 */
C_TcpSocketResultCode galay_kernel_tcp_socket_local_endpoint(
    const galay_kernel_tcp_socket_t* c_socket,
    C_Host* endpoint);

/**
 * @brief 挂起当前 C coroutine 并接受一个 TCP 连接。
 *
 * @param listener 已 bind/listen 的 TCP socket。
 * @param out_socket 成功时接收 accepted socket；调用前 out_socket->socket 必须为 NULL，
 * 调用方负责 destroy。
 * @param out_peer 可选输出 peer 地址；不需要时传 NULL。
 * @param timeout_ms 负数无限等待，0 不提交 accept 并直接返回超时，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk，result.ptr 指向 out_socket，result.value 保存底层
 * accepted fd；超时返回 C_IOResultTimeout；参数、状态、非 C coroutine/IO scheduler
 * 上下文无效返回 C_IOResultInvalid；底层 I/O 失败返回 C_IOResultError。
 *
 * @note 该函数必须在 galay_coro_spawn 创建并运行于 IO scheduler 的 C coroutine 内
 * 调用。listener、out_socket 和 out_peer 必须在函数返回前保持有效。close 可取消
 * 同一 controller 上挂起的 direct C coroutine 操作。
 */
C_IOResult galay_kernel_tcp_socket_accept(
    galay_kernel_tcp_socket_t* listener,
    galay_kernel_tcp_socket_t* out_socket,
    C_Host* out_peer,
    int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并连接远端 TCP 地址。
 *
 * @param socket 已创建的 TCP socket。
 * @param host 远端地址和端口，必须为有效 IPv4/IPv6 host。
 * @param timeout_ms 负数无限等待，0 立即返回 C_IOResultTimeout，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk；超时返回 C_IOResultTimeout；参数、状态或 coroutine
 * 上下文无效返回 C_IOResultInvalid；底层连接失败返回 C_IOResultError。
 *
 * @note 该函数会在挂起前确保 socket 为非阻塞模式。socket 和 host 必须在函数返回前
 * 保持有效。
 */
C_IOResult galay_kernel_tcp_socket_connect(
    galay_kernel_tcp_socket_t* socket,
    const C_Host* host,
    int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并从 TCP socket 接收数据。
 *
 * @param socket 已连接或已接受的 TCP socket。
 * @param buffer 调用方提供的输出缓冲区，必须非 NULL。
 * @param length 最多接收字节数，必须大于 0。
 * @param timeout_ms 负数无限等待，0 立即返回 C_IOResultTimeout，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk，result.bytes 为接收字节数；对端断开返回
 * C_IOResultEof；超时返回 C_IOResultTimeout；参数或上下文无效返回
 * C_IOResultInvalid；底层错误返回 C_IOResultError。
 *
 * @note buffer 在 coroutine 挂起期间由底层 I/O 写入，调用方必须保证其直到函数返回
 * 前保持有效且不被并发修改。
 */
C_IOResult galay_kernel_tcp_socket_recv(
    galay_kernel_tcp_socket_t* socket,
    char* buffer,
    size_t length,
    int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并向 TCP socket 发送数据。
 *
 * @param socket 已连接或已接受的 TCP socket。
 * @param buffer 待发送数据，必须非 NULL。
 * @param length 待发送字节数，必须大于 0。
 * @param timeout_ms 负数无限等待，0 立即返回 C_IOResultTimeout，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk，result.bytes 为发送字节数；对端断开可返回
 * C_IOResultEof；超时返回 C_IOResultTimeout；参数或上下文无效返回
 * C_IOResultInvalid；底层错误返回 C_IOResultError。
 *
 * @note buffer 必须在函数返回前保持有效。一次调用不保证发送完整 length 字节，
 * 调用方需要根据 result.bytes 处理短写。
 */
C_IOResult galay_kernel_tcp_socket_send(
    galay_kernel_tcp_socket_t* socket,
    const char* buffer,
    size_t length,
    int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并执行 scatter read。
 *
 * @param socket 已连接或已接受的 TCP socket。
 * @param iovecs 调用方提供的 iovec 数组，必须非 NULL。
 * @param count iovec 数量，必须大于 0。
 * @param timeout_ms 负数无限等待，0 立即返回 C_IOResultTimeout，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk，result.bytes 为读取字节数；其它结果码语义同 recv。
 *
 * @note iovecs 以及其中每个 iov_base 指向的缓冲区必须在函数返回前保持有效。
 */
C_IOResult galay_kernel_tcp_socket_readv(
    galay_kernel_tcp_socket_t* socket,
    const struct iovec* iovecs,
    size_t count,
    int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并执行 gather write。
 *
 * @param socket 已连接或已接受的 TCP socket。
 * @param iovecs 待发送 iovec 数组，必须非 NULL。
 * @param count iovec 数量，必须大于 0。
 * @param timeout_ms 负数无限等待，0 立即返回 C_IOResultTimeout，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk，result.bytes 为写入字节数；其它结果码语义同 send。
 *
 * @note iovecs 以及其中每个 iov_base 指向的数据必须在函数返回前保持有效。调用方
 * 需要处理短写。
 */
C_IOResult galay_kernel_tcp_socket_writev(
    galay_kernel_tcp_socket_t* socket,
    const struct iovec* iovecs,
    size_t count,
    int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并通过 sendfile 发送文件内容。
 *
 * @param socket 已连接或已接受的 TCP socket。
 * @param file_fd 待读取文件描述符，必须非负，调用方保持所有权。
 * @param offset 文件偏移，必须非负。
 * @param count 最多发送字节数，必须大于 0。
 * @param timeout_ms 负数无限等待，0 立即返回 C_IOResultTimeout，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk，result.bytes 为发送字节数；超时、无效参数和底层
 * 错误分别返回 C_IOResultTimeout/C_IOResultInvalid/C_IOResultError。
 *
 * @note file_fd 和 socket 必须在函数返回前保持有效。调用方仍拥有 file_fd，并负责
 * 在合适时机关闭。
 */
C_IOResult galay_kernel_tcp_socket_sendfile(
    galay_kernel_tcp_socket_t* socket,
    int file_fd,
    int64_t offset,
    size_t count,
    int64_t timeout_ms);

/**
 * @brief 在当前 IO scheduler 上关闭 TCP socket。
 *
 * @param socket 已创建的 TCP socket。
 * @param timeout_ms 为 ABI 对称保留；当前实现只校验取值是否可转换为内部 timeout。
 * @return 成功返回 C_IOResultOk；参数、非 IO scheduler 上下文、存在非 direct C
 * coroutine 挂起操作或 controller 所属 scheduler 不匹配时返回 C_IOResultInvalid；
 * 底层 close 注册失败返回 C_IOResultError。
 *
 * @note 该函数必须在 C coroutine 的 IO scheduler 上调用。close 会取消同一 socket
 * 上挂起的 direct C coroutine I/O 操作，使其以 C_IOResultCancelled 完成。
 */
C_IOResult galay_kernel_tcp_socket_close(
    galay_kernel_tcp_socket_t* socket,
    int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
