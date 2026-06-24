#ifndef GALAY_KERNEL_ASYNC_MUTEX_C_H
#define GALAY_KERNEL_ASYNC_MUTEX_C_H

#include "../core-c/runtime_c.h"
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
    C_AsyncMutexRuntimeNotRunning,      ///< runtime 未启动。
    C_AsyncMutexRuntimeSpawnFailed,     ///< runtime 提交任务失败。
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
 * @brief AsyncMutex lock 完成回调。
 *
 * @param code lock 完成结果码；获取锁成功为 C_AsyncMutexSuccess，超时为 C_AsyncMutexTimeout。
 * @param ctx 调用 lock API 时传入的用户上下文。
 */
typedef void (*galay_kernel_async_mutex_callback_t)(
    C_AsyncMutexResultCode code,
    void* ctx);

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
 *       调用方必须确保没有未完成的 lock/lock_timeout 回调仍可能访问该 mutex。
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
 * @brief 在 runtime 上异步获取 AsyncMutex。
 *
 * @param runtime 用于驱动 lock 协程的 runtime；必须存活到 callback 完成。
 * @param c_mutex AsyncMutex 句柄；必须存活到 callback 完成。
 * @param callback lock 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交 lock 操作返回 C_AsyncMutexSuccess；参数无效返回 C_AsyncMutexParameterInvalid；
 * runtime 未运行返回 C_AsyncMutexRuntimeNotRunning；提交失败返回 C_AsyncMutexRuntimeSpawnFailed。
 *
 * @note 该函数不会阻塞等待锁；最终获取结果通过 callback 上报，callback 在 runtime 调度线程上执行。
 *       callback 收到 C_AsyncMutexSuccess 后，调用方负责在临界区结束时调用 unlock。
 */
C_AsyncMutexResultCode galay_kernel_async_mutex_lock(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_mutex_t* c_mutex,
    galay_kernel_async_mutex_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步获取 AsyncMutex，并设置超时。
 *
 * @param runtime 用于驱动 lock 协程的 runtime；必须存活到 callback 完成。
 * @param c_mutex AsyncMutex 句柄；必须存活到 callback 完成。
 * @param timeout_ms 超时时间，单位毫秒。
 * @param callback lock 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交 lock 操作返回 C_AsyncMutexSuccess；参数无效返回 C_AsyncMutexParameterInvalid；
 * runtime 未运行返回 C_AsyncMutexRuntimeNotRunning；提交失败返回 C_AsyncMutexRuntimeSpawnFailed。
 *
 * @note 该函数不会阻塞等待锁；超时通过 callback 返回 C_AsyncMutexTimeout。
 *       callback 收到 C_AsyncMutexSuccess 后，调用方负责在临界区结束时调用 unlock。
 */
C_AsyncMutexResultCode galay_kernel_async_mutex_lock_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_mutex_t* c_mutex,
    uint64_t timeout_ms,
    galay_kernel_async_mutex_callback_t callback,
    void* ctx);

#ifdef __cplusplus
}
#endif

#endif
