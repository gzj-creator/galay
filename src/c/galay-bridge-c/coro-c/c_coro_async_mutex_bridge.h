#ifndef GALAY_KERNEL_CORE_C_CORO_ASYNC_MUTEX_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_ASYNC_MUTEX_BRIDGE_H

#include "c_coro_tcp_bridge.h"

/**
 * @file c_coro_async_mutex_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 AsyncMutex adapter。
 *
 * @details 该内部 bridge 把 AsyncMutex awaitable 细节限制在 kernel core 内；C ABI
 * wrapper 提供 wait token 并在当前 C coroutine 中等待锁可用。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂起当前 C coroutine 直到 AsyncMutex 加锁成功或超时。
 *
 * @param mutex 内部 AsyncMutex 指针，必须非 NULL。
 * @param scheduler 当前 C coroutine 所属 scheduler，必须非 NULL。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL，由 wait_ops 管理完成和释放。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功获得锁返回 Ok；timeout_ms 为 0 或等待超时返回 Timeout；参数、
 * scheduler、wait hook 或 token 无效返回 Invalid；分配 wake state 失败返回
 * Error/ENOMEM。
 *
 * @note user_data 必须在函数返回或清理完成前保持有效。成功返回后底层 AsyncMutex
 * 的解锁责任仍由拥有该 mutex 的上层 API/调用方承担；bridge 本身不提供 unlock。
 * wait 回调应挂起 coroutine，不应阻塞 scheduler 线程。
 */
GalayCoreCoroIOResult galay_core_coro_async_mutex_lock(
    GalayCoreAsyncMutex* mutex,
    GalayCoreIOScheduler* scheduler,
    int64_t timeout_ms,
    void* user_data,
    const GalayCoreCoroWaitOps* wait_ops);

#ifdef __cplusplus
}
#endif

#endif
