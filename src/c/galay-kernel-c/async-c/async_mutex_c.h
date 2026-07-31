#ifndef GALAY_KERNEL_ASYNC_MUTEX_C_H
#define GALAY_KERNEL_ASYNC_MUTEX_C_H

#include "../coro-c/coro_result_c.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @file async_mutex_c.h
 * @brief Galay kernel AsyncMutex 的 C ABI 封装。
 *
 * @details 该头文件只暴露 C 可见的轻量句柄和状态码，实际 mutex 生命周期
 * 由实现文件中的 C++ galay::kernel::AsyncMutex 承载。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AsyncMutex C ABI 操作结果码。
 */
typedef enum C_AsyncMutexResultCode {
    C_AsyncMutexSuccess,                ///< 操作成功。
    C_AsyncMutexParameterInvalid,       ///< 参数错误。
    C_AsyncMutexMemoryAllocFailed,      ///< 内存分配失败。
    C_AsyncMutexIOFailed,               ///< 底层异步等待失败。
    C_AsyncMutexOperationInvalid,       ///< 当前 mutex 状态不允许执行该操作。
    C_AsyncMutexTimeout,                ///< 异步 lock 超时。
} C_AsyncMutexResultCode;

/**
 * @brief AsyncMutex C 句柄。
 *
 * @note mutex 指向内部 C++ AsyncMutex 对象，调用方不能解引用或直接释放。
 */
typedef struct galay_kernel_async_mutex {
    void* mutex;            ///< 内部 AsyncMutex 对象指针。
} galay_kernel_async_mutex_t;

/**
 * @brief 将 AsyncMutex 结果码转换为可读错误信息。
 *
 * @param code C_AsyncMutexResultCode 结果码。
 * @return 指向静态错误字符串的指针，调用方不需要释放。
 */
const char* galay_kernel_async_mutex_get_error(C_AsyncMutexResultCode code);

/**
 * @brief 创建 AsyncMutex。
 *
 * @param c_mutex 输出 mutex 句柄；成功时其 mutex 字段指向内部 AsyncMutex。
 * @return 成功返回 C_AsyncMutexSuccess；参数无效返回 C_AsyncMutexParameterInvalid；
 * 内存分配失败返回 C_AsyncMutexMemoryAllocFailed。
 *
 * @note 该函数只创建锁对象，不启动协程，也不会阻塞。
 */
C_AsyncMutexResultCode galay_kernel_async_mutex_create(galay_kernel_async_mutex_t* c_mutex);

/**
 * @brief 销毁 AsyncMutex 内部资源。
 *
 * @param c_mutex 由 galay_kernel_async_mutex_create 初始化的 mutex 句柄。
 * @return 成功返回 C_AsyncMutexSuccess；参数无效返回 C_AsyncMutexParameterInvalid。
 *
 * @note 该函数会释放 c_mutex->mutex 指向的内部 AsyncMutex，并将其置空。
 *       c_mutex 非空且 mutex 字段已为 NULL 时也安全返回成功。
 *       调用方必须确保没有未完成的 direct C coroutine lock 仍可能访问该 mutex。
 */
C_AsyncMutexResultCode galay_kernel_async_mutex_destroy(galay_kernel_async_mutex_t* c_mutex);

/**
 * @brief 观察当前锁状态。
 *
 * @param c_mutex AsyncMutex 句柄；为空或未初始化时返回 false。
 * @return true 表示当前有路径持锁；false 表示未持锁或参数无效。
 *
 * @note 该结果只适合诊断/断言，不应用作无竞争同步条件。
 */
bool galay_kernel_async_mutex_is_locked(const galay_kernel_async_mutex_t* c_mutex);

/**
 * @brief 同步释放 AsyncMutex。
 *
 * @param c_mutex 由 galay_kernel_async_mutex_create 初始化的 mutex 句柄。
 * @return 成功返回 C_AsyncMutexSuccess；参数无效返回 C_AsyncMutexParameterInvalid。
 *
 * @note C ABI 无法验证当前调用路径是否为持锁 owner；调用方必须保证仅由持锁路径调用。
 *       该函数不会阻塞，会唤醒一个仍有效的等待协程。
 */
C_AsyncMutexResultCode galay_kernel_async_mutex_unlock(galay_kernel_async_mutex_t* c_mutex);

/**
 * @brief 挂起当前 C coroutine 并获取 AsyncMutex。
 *
 * @param c_mutex AsyncMutex 句柄。
 * @param timeout_ms 负数无限等待，0 立即返回超时，正数为毫秒超时。
 * @return 成功获取锁返回 C_IOResultOk；超时返回 C_IOResultTimeout；参数无效、
 * 不在 C coroutine 内调用或 mutex 状态无效返回 C_IOResultInvalid。
 *
 * @note 成功返回后调用方负责在临界区结束时调用 unlock。
 */
C_IOResult galay_kernel_async_mutex_lock(
    galay_kernel_async_mutex_t* c_mutex,
    int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
