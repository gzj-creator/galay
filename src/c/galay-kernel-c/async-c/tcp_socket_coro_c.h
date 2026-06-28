#ifndef GALAY_KERNEL_TCP_SOCKET_CORO_C_H
#define GALAY_KERNEL_TCP_SOCKET_CORO_C_H

#include "tcp_socket_c.h"
#include "../coro-c/coro_result_c.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @file tcp_socket_coro_c.h
 * @brief Galay kernel TCP socket 的 direct C coroutine API。
 *
 * @details 这些接口必须在 `galay_coro_spawn` 创建的 C coroutine 内调用。
 * 它们复用现有 TCP socket 句柄和 galay IO scheduler，挂起当前 C coroutine，
 * 不会为每次 I/O 操作创建 C++ `Task<void>` 桥接任务。
 *
 * @note Task 6 的 direct TCP 适配器一次只允许同一 socket 上存在一个
 * pending direct C coroutine I/O 操作。这样可以复用当前 fd 级别的
 * timeout/remove 后端语义，避免一个方向的超时误取消另一个方向。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂起当前 C coroutine 并接受一个 TCP 连接。
 * @param listener 已 bind/listen 的 TCP socket。
 * @param out_socket 成功时接收 accepted socket；调用前必须为空，调用方负责 destroy。
 * @param timeout_ms 负数无限等待，0 不提交 accept 并直接返回超时，正数为毫秒超时。
 * @return 成功返回 `C_IOResultOk`，`ptr` 指向 out_socket，`value` 为 accepted fd；
 * 超时返回 `C_IOResultTimeout`，参数/上下文错误返回 `C_IOResultInvalid`。
 */
C_IOResult galay_coro_tcp_accept(galay_kernel_tcp_socket_t* listener,
                                 galay_kernel_tcp_socket_t* out_socket,
                                 int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并连接远端 TCP 地址。
 * @param socket 已创建的 TCP socket。
 * @param host 远端地址；调用期间会被复制，不借用。
 * @param timeout_ms 负数无限等待，0 不提交 connect 并直接返回超时，正数为毫秒超时。
 * @return 成功返回 `C_IOResultOk`；失败通过 `code/sys_errno` 返回。
 * @note 正 timeout 返回 `C_IOResultTimeout` 时，底层 fd 会在所属 IO scheduler
 * 上关闭，调用方必须 destroy 当前 C 句柄并重新 create 后才能再次连接。
 */
C_IOResult galay_coro_tcp_connect(galay_kernel_tcp_socket_t* socket,
                                  const C_Host* host,
                                  int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并读取一次 TCP 数据。
 * @param socket 已连接的 TCP socket。
 * @param buffer 接收缓冲区；必须在调用返回前保持有效。
 * @param length 缓冲区长度，必须大于 0。
 * @param timeout_ms 负数无限等待，0 不提交 recv 并直接返回超时，正数为毫秒超时。
 * @return 成功返回 `C_IOResultOk` 且 `bytes` 为读取字节数；远端关闭返回
 * `C_IOResultEof`。
 */
C_IOResult galay_coro_tcp_recv(galay_kernel_tcp_socket_t* socket,
                               char* buffer,
                               size_t length,
                               int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并写入一次 TCP 数据。
 * @param socket 已连接的 TCP socket。
 * @param buffer 待发送数据；必须在调用返回前保持有效。
 * @param length 数据长度，必须大于 0。
 * @param timeout_ms 负数无限等待，0 不提交 send 并直接返回超时，正数为毫秒超时。
 * @return 成功返回 `C_IOResultOk` 且 `bytes` 为实际写入字节数。
 * @note 正 timeout 返回 `C_IOResultTimeout` 只表示当前 C coroutine 停止等待
 * 本次 send 完成；内核可能已经接受并向 peer 暴露部分字节。调用方若需要
 * “peer 一定不可见”的语义，必须在应用协议层使用幂等/序列号确认。
 */
C_IOResult galay_coro_tcp_send(galay_kernel_tcp_socket_t* socket,
                               const char* buffer,
                               size_t length,
                               int64_t timeout_ms);

/**
 * @brief 在当前 C coroutine 所属 IO scheduler 上关闭 TCP socket。
 * @param socket TCP socket。
 * @param timeout_ms 保留参数；当前实现忽略该值，负数、0、正数行为相同。
 * @return 成功返回 `C_IOResultOk`；若该 socket 有 direct C coroutine I/O 正在等待，
 * 等待者会收到 `C_IOResultCancelled`。
 * @note 当前 close 合约是同步提交关闭/取消动作后立即返回，不挂起当前 coroutine，
 * 不等待内核关闭完成，也不会因为传入正 timeout 而阻塞到超时结束。
 */
C_IOResult galay_coro_tcp_close(galay_kernel_tcp_socket_t* socket,
                                int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
