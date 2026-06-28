#ifndef GALAY_KERNEL_ASYNC_WAITER_C_H
#define GALAY_KERNEL_ASYNC_WAITER_C_H

#include "../coro-c/coro_result_c.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @file async_waiter_c.h
 * @brief Galay kernel AsyncWaiter<void> 的 C ABI 封装。
 *
 * @details 该头文件只封装不携带返回值的 AsyncWaiter<void>。每个 waiter
 * 实例按底层 C++ 语义只能完成一次 wait/notify 信号流程；调用方必须保证
 * waiter 和 runtime 在异步 callback 完成前保持有效。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AsyncWaiter C ABI 操作结果码。
 */
typedef enum C_AsyncWaiterResultCode {
    C_AsyncWaiterSuccess,                ///< 操作成功。
    C_AsyncWaiterParameterInvalid,       ///< 参数错误。
    C_AsyncWaiterMemoryAllocFailed,      ///< 内存分配失败。
    C_AsyncWaiterIOFailed,               ///< 底层异步等待失败。
    C_AsyncWaiterOperationInvalid,       ///< 当前 waiter 状态不允许执行该操作。
    C_AsyncWaiterTimeout,                ///< 等待超时。
} C_AsyncWaiterResultCode;

/**
 * @brief AsyncWaiter<void> C 句柄。
 *
 * @note waiter 指向内部 C++ galay::kernel::AsyncWaiter<void> 对象，调用方不能
 * 解引用或直接释放。
 */
typedef struct galay_kernel_async_waiter {
    void* waiter;     ///< 内部 AsyncWaiter<void> 对象指针。
} galay_kernel_async_waiter_t;

/**
 * @brief 将 AsyncWaiter 结果码转换为可读错误信息。
 *
 * @param code C_AsyncWaiterResultCode 结果码。
 * @return 指向静态错误字符串的指针，调用方不需要释放。
 */
const char* galay_kernel_async_waiter_get_error(C_AsyncWaiterResultCode code);

/**
 * @brief 创建 AsyncWaiter<void>。
 *
 * @param c_waiter 输出 waiter 句柄；成功时其 waiter 字段指向内部 AsyncWaiter<void>。
 * @return 成功返回 C_AsyncWaiterSuccess；参数无效返回 C_AsyncWaiterParameterInvalid；
 * 内存分配失败返回 C_AsyncWaiterMemoryAllocFailed。
 *
 * @note 该函数只创建等待器对象，不启动协程。
 */
C_AsyncWaiterResultCode galay_kernel_async_waiter_create(galay_kernel_async_waiter_t* c_waiter);

/**
 * @brief 销毁 AsyncWaiter<void> 内部资源。
 *
 * @param c_waiter 由 galay_kernel_async_waiter_create 初始化的 waiter 句柄。
 * @return 成功返回 C_AsyncWaiterSuccess；参数无效返回 C_AsyncWaiterParameterInvalid。
 *
 * @note 调用方必须保证没有未完成的 direct C coroutine wait 再访问该 waiter。
 * 该函数会释放 c_waiter->waiter 指向的内部对象，并将其置空。
 */
C_AsyncWaiterResultCode galay_kernel_async_waiter_destroy(galay_kernel_async_waiter_t* c_waiter);

/**
 * @brief 查询 waiter 是否已有协程挂起等待。
 *
 * @param c_waiter waiter 句柄。
 * @return 正在等待返回 true；句柄无效或未等待返回 false。
 */
bool galay_kernel_async_waiter_is_waiting(const galay_kernel_async_waiter_t* c_waiter);

/**
 * @brief 查询 waiter 是否已收到完成通知。
 *
 * @param c_waiter waiter 句柄。
 * @return 已 ready 返回 true；句柄无效或未 ready 返回 false。
 */
bool galay_kernel_async_waiter_is_ready(const galay_kernel_async_waiter_t* c_waiter);

/**
 * @brief 发送完成通知并唤醒等待协程。
 *
 * @param c_waiter waiter 句柄。
 * @return 首次通知成功返回 C_AsyncWaiterSuccess；参数无效返回
 * C_AsyncWaiterParameterInvalid；重复 notify 返回 C_AsyncWaiterOperationInvalid。
 *
 * @note AsyncWaiter<void> 是单次使用对象，成功 notify 后不能再次 notify。
 */
C_AsyncWaiterResultCode galay_kernel_async_waiter_notify(galay_kernel_async_waiter_t* c_waiter);

/**
 * @brief 挂起当前 C coroutine 并等待完成通知。
 *
 * @param c_waiter waiter 句柄。
 * @param timeout_ms 负数无限等待，0 立即返回超时，正数为毫秒超时。
 * @return 成功收到通知返回 C_IOResultOk；超时返回 C_IOResultTimeout；参数无效、
 * 不在 C coroutine 内调用或 waiter 状态无效返回 C_IOResultInvalid。
 *
 * @note 每个 waiter 实例只支持一次 wait/notify 或 wait timeout 信号流程。
 */
C_IOResult galay_kernel_async_waiter_wait(
    galay_kernel_async_waiter_t* c_waiter,
    int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
