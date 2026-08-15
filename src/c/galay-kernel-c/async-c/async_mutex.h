#ifndef GALAY_C_KERNEL_ASYNC_ASYNC_MUTEX_H
#define GALAY_C_KERNEL_ASYNC_ASYNC_MUTEX_H

#include "../coro-c/coro_result.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum C_AsyncMutexResultCode {
    C_AsyncMutexSuccess,
    C_AsyncMutexParameterInvalid,
    C_AsyncMutexMemoryAllocFailed,
    C_AsyncMutexOperationInvalid,
} C_AsyncMutexResultCode;

typedef struct galay_c_async_mutex {
    void* mutex;
} galay_c_async_mutex_t;

const char* galay_c_async_mutex_get_error(C_AsyncMutexResultCode code);
C_AsyncMutexResultCode galay_c_async_mutex_create(galay_c_async_mutex_t* mutex);
C_AsyncMutexResultCode galay_c_async_mutex_destroy(galay_c_async_mutex_t* mutex);
bool galay_c_async_mutex_is_locked(const galay_c_async_mutex_t* mutex);
C_AsyncMutexResultCode galay_c_async_mutex_unlock(galay_c_async_mutex_t* mutex);

/**
 * @brief Acquires the mutex from the current C coroutine.
 * @param mutex Initialized mutex handle.
 * @param timeout_ms -1 waits indefinitely, 0 polls, and a positive value sets a deadline.
 * @return An explicit coroutine result. This function suspends rather than blocking the scheduler.
 */
C_IOResult galay_c_async_mutex_lock(galay_c_async_mutex_t* mutex,
                                         int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
