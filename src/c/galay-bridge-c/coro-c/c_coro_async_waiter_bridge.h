#ifndef GALAY_KERNEL_CORE_C_CORO_ASYNC_WAITER_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_ASYNC_WAITER_BRIDGE_H

#include "c_coro_tcp_bridge.h"

/**
 * @file c_coro_async_waiter_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 AsyncWaiter<void> adapter。
 *
 * @details 该内部 bridge 让 C coroutine runtime 通过 wait token 等待 C++
 * AsyncWaiter<void> 完成，同时把 C++ awaitable/waker 细节限制在 kernel core 内。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂起当前 C coroutine 直到 AsyncWaiter<void> 被唤醒或超时。
 *
 * @param waiter 内部 AsyncWaiter<void> 指针，必须非 NULL。
 * @param scheduler 当前 C coroutine 所属 scheduler，必须非 NULL。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL，由 wait_ops 管理完成和释放。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return waiter 已就绪或被唤醒返回 Ok；timeout_ms 为 0 或等待超时返回 Timeout；
 * 参数、scheduler、wait hook 或 token 无效返回 Invalid；底层等待错误返回
 * Error/sys_errno。
 *
 * @note user_data 必须在函数返回或清理完成前保持有效。wait 回调应挂起 coroutine，
 * 不应阻塞 scheduler 线程。
 */
GalayCoreCoroIOResult galay_core_coro_async_waiter_wait(
    void* waiter,
    void* scheduler,
    int64_t timeout_ms,
    void* user_data,
    const GalayCoreCoroWaitOps* wait_ops);

#ifdef __cplusplus
}
#endif

#endif
