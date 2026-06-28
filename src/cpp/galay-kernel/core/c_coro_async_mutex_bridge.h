#ifndef GALAY_KERNEL_CORE_C_CORO_ASYNC_MUTEX_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_ASYNC_MUTEX_BRIDGE_H

#include "c_coro_tcp_bridge.h"

/**
 * @file c_coro_async_mutex_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 AsyncMutex adapter。
 */

#ifdef __cplusplus
extern "C" {
#endif

GalayCoreCoroIOResult galay_core_coro_async_mutex_lock(
    void* mutex,
    void* scheduler,
    int64_t timeout_ms,
    void* user_data,
    const GalayCoreCoroWaitOps* wait_ops);

#ifdef __cplusplus
}
#endif

#endif
