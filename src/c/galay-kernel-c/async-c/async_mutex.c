#include "async_mutex.h"

#include "../coro-c/coro_task_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

typedef struct galay_c_async_mutex_impl {
    _Atomic int locked;
} galay_c_async_mutex_impl_t;

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0
        ? (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000
        : -1;
}

const char* galay_c_async_mutex_get_error(C_AsyncMutexResultCode code)
{
    switch (code) {
    case C_AsyncMutexSuccess:
        return "success";
    case C_AsyncMutexParameterInvalid:
        return "parameter invalid";
    case C_AsyncMutexMemoryAllocFailed:
        return "memory allocation failed";
    case C_AsyncMutexOperationInvalid:
        return "operation invalid";
    }
    return "unknown async mutex error";
}

C_AsyncMutexResultCode galay_c_async_mutex_create(galay_c_async_mutex_t* mutex)
{
    if (mutex == NULL || mutex->mutex != NULL) {
        return C_AsyncMutexParameterInvalid;
    }
    galay_c_async_mutex_impl_t* const impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return C_AsyncMutexMemoryAllocFailed;
    }
    atomic_init(&impl->locked, 0);
    mutex->mutex = impl;
    return C_AsyncMutexSuccess;
}

C_AsyncMutexResultCode galay_c_async_mutex_destroy(galay_c_async_mutex_t* mutex)
{
    if (mutex == NULL) {
        return C_AsyncMutexParameterInvalid;
    }
    if (mutex->mutex == NULL) {
        return C_AsyncMutexSuccess;
    }
    galay_c_async_mutex_impl_t* const impl = mutex->mutex;
    if (atomic_load_explicit(&impl->locked, memory_order_acquire) != 0) {
        return C_AsyncMutexOperationInvalid;
    }
    free(impl);
    mutex->mutex = NULL;
    return C_AsyncMutexSuccess;
}

bool galay_c_async_mutex_is_locked(const galay_c_async_mutex_t* mutex)
{
    if (mutex == NULL || mutex->mutex == NULL) {
        return false;
    }
    const galay_c_async_mutex_impl_t* const impl = mutex->mutex;
    return atomic_load_explicit(&impl->locked, memory_order_acquire) != 0;
}

C_AsyncMutexResultCode galay_c_async_mutex_unlock(galay_c_async_mutex_t* mutex)
{
    if (mutex == NULL || mutex->mutex == NULL) {
        return C_AsyncMutexParameterInvalid;
    }
    galay_c_async_mutex_impl_t* const impl = mutex->mutex;
    return atomic_exchange_explicit(&impl->locked, 0, memory_order_acq_rel) != 0
        ? C_AsyncMutexSuccess
        : C_AsyncMutexOperationInvalid;
}

C_IOResult galay_c_async_mutex_lock(galay_c_async_mutex_t* mutex,
                                         int64_t timeout_ms)
{
    if (mutex == NULL || mutex->mutex == NULL || timeout_ms < -1 ||
        galay_c_coro_task_current_internal() == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    galay_c_async_mutex_impl_t* const impl = mutex->mutex;
    const int64_t start = monotonic_milliseconds();
    if (start < 0) {
        return make_result(C_IOResultError, errno);
    }
    for (;;) {
        int expected = 0;
        if (atomic_compare_exchange_weak_explicit(&impl->locked, &expected, 1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return make_result(C_IOResultOk, 0);
        }
        if (timeout_ms == 0 ||
            (timeout_ms > 0 && monotonic_milliseconds() - start >= timeout_ms)) {
            return make_result(C_IOResultTimeout, 0);
        }
        const C_IOResult yielded = galay_c_coro_yield();
        if (yielded.code != C_IOResultOk) {
            return yielded;
        }
    }
}
