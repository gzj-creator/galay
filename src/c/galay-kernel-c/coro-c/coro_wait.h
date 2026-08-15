#ifndef GALAY_C_KERNEL_CORO_WAIT_H
#define GALAY_C_KERNEL_CORO_WAIT_H

/**
 * @file coro_wait.h
 * @brief 原生 C 协程 I/O 等待原语，直接对接 native scheduler，无 C++ bridge。
 *
 * @details 提供零开销的协程 I/O 等待：
 * - 无 shared_ptr、无动态分配
 * - 单次原子 CAS 完成注册和唤醒
 * - generation-based validation 防止 ABA 问题
 *
 * 使用流程：
 *   1. 尝试立即非阻塞 I/O（recv/send with MSG_DONTWAIT）
 *   2. 如果返回 EAGAIN，调用 galay_c_coro_wait_io 挂起
 *   3. Reactor 唤醒后，协程恢复并重试 I/O
 */

#include <galay/c/galay-kernel-c/core-c/io_controller.h>
#include <galay/c/galay-kernel-c/core-c/io_scheduler.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂起当前 C 协程等待 I/O 事件。
 *
 * @param scheduler 当前协程所属的 IO scheduler。
 * @param controller 待等待的 IO controller。
 * @param event_type 等待的事件类型（GALAY_C_EVENT_READ 或 GALAY_C_EVENT_WRITE）。
 * @param timeout_ms 超时毫秒数；负数表示无限等待，0 表示立即返回，正数表示最多等待指定毫秒。
 * @return I/O 就绪返回 C_IOResultOk；超时返回 C_IOResultTimeout；参数无效或状态错误返回 C_IOResultInvalid。
 *
 * @note 该函数只能在 C 协程内调用。它会：
 *   1. 将当前协程注册到 controller 的对应槽位
 *   2. 更新 reactor 注册（epoll_ctl MOD/ADD）
 *   3. context switch 挂起当前协程
 *   4. 恢复后返回（由 reactor 或 timeout 唤醒）
 *
 * 调用方责任：
 *   - 在返回 Ok 后重试非阻塞 I/O
 *   - 处理 Timeout/Invalid 错误
 */
C_IOResult galay_c_coro_wait_io(galay_c_io_scheduler_t* scheduler,
                                galay_c_io_controller_t* controller,
                                uint32_t event_type,
                                int64_t timeout_ms);

/**
 * @brief 取消挂起在 controller 上的协程。
 *
 * @param controller IO controller。
 * @param event_type 待取消的事件类型（GALAY_C_EVENT_READ 或 GALAY_C_EVENT_WRITE）。
 * @return 成功取消返回 C_IOResultCancelled；槽位为空返回 C_IOResultInvalid。
 *
 * @note 该函数会将挂起的协程标记为 Cancelled 并入队到 ready queue。
 * 用于 close/shutdown 等场景。
 */
C_IOResult galay_c_coro_cancel_io(galay_c_io_scheduler_t* scheduler,
                                  galay_c_io_controller_t* controller,
                                  uint32_t event_type);

/**
 * @brief 立即尝试非阻塞 recv，失败则挂起等待。
 *
 * @param scheduler 当前协程所属的 IO scheduler。
 * @param controller 待读取的 IO controller。
 * @param buffer 接收缓冲区。
 * @param length 最多接收字节数。
 * @param timeout_ms 超时毫秒数。
 * @return 成功返回 C_IOResultOk 且 bytes 字段为读取字节数；EOF 返回 C_IOResultEof；
 * 超时返回 C_IOResultTimeout；错误返回 C_IOResultError/errno。
 *
 * @note 这是一个便利函数，封装了"立即尝试 → 等待 → 重试"的完整流程。
 */
C_IOResult galay_c_coro_recv_blocking(galay_c_io_scheduler_t* scheduler,
                                      galay_c_io_controller_t* controller,
                                      char* buffer,
                                      size_t length,
                                      int64_t timeout_ms);

/**
 * @brief 立即尝试非阻塞 send，失败则挂起等待。
 *
 * @param scheduler 当前协程所属的 IO scheduler。
 * @param controller 待写入的 IO controller。
 * @param buffer 待发送数据。
 * @param length 待发送字节数。
 * @param timeout_ms 超时毫秒数。
 * @return 成功返回 C_IOResultOk 且 bytes 字段为发送字节数；
 * 超时返回 C_IOResultTimeout；错误返回 C_IOResultError/errno。
 *
 * @note 调用方需要处理短写（partial write）。
 */
C_IOResult galay_c_coro_send_blocking(galay_c_io_scheduler_t* scheduler,
                                      galay_c_io_controller_t* controller,
                                      const char* buffer,
                                      size_t length,
                                      int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
