#include "async_waiter.h"

#include "../coro-c/coro_task_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

typedef struct galay_c_async_waiter_impl {
    _Atomic int ready;
    _Atomic(C_CoroTaskInternal*) task;
} galay_c_async_waiter_impl_t;

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

const char* galay_c_async_waiter_get_error(C_AsyncWaiterResultCode code)
{
    switch (code) {
    case C_AsyncWaiterSuccess:
        return "success";
    case C_AsyncWaiterParameterInvalid:
        return "parameter invalid";
    case C_AsyncWaiterMemoryAllocFailed:
        return "memory allocation failed";
    case C_AsyncWaiterOperationInvalid:
        return "operation invalid";
    }
    return "unknown async waiter error";
}

C_AsyncWaiterResultCode galay_c_async_waiter_create(galay_c_async_waiter_t* waiter)
{
    if (waiter == NULL || waiter->waiter != NULL) {
        return C_AsyncWaiterParameterInvalid;
    }
    galay_c_async_waiter_impl_t* const impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return C_AsyncWaiterMemoryAllocFailed;
    }
    atomic_init(&impl->ready, 0);
    atomic_init(&impl->task, NULL);
    waiter->waiter = impl;
    return C_AsyncWaiterSuccess;
}

C_AsyncWaiterResultCode galay_c_async_waiter_destroy(galay_c_async_waiter_t* waiter)
{
    if (waiter == NULL) {
        return C_AsyncWaiterParameterInvalid;
    }
    if (waiter->waiter == NULL) {
        return C_AsyncWaiterSuccess;
    }
    galay_c_async_waiter_impl_t* const impl = waiter->waiter;
    if (atomic_load_explicit(&impl->task, memory_order_acquire) != NULL) {
        return C_AsyncWaiterOperationInvalid;
    }
    free(impl);
    waiter->waiter = NULL;
    return C_AsyncWaiterSuccess;
}

bool galay_c_async_waiter_is_waiting(const galay_c_async_waiter_t* waiter)
{
    if (waiter == NULL || waiter->waiter == NULL) {
        return false;
    }
    const galay_c_async_waiter_impl_t* const impl = waiter->waiter;
    return atomic_load_explicit(&impl->task, memory_order_acquire) != NULL;
}

bool galay_c_async_waiter_is_ready(const galay_c_async_waiter_t* waiter)
{
    if (waiter == NULL || waiter->waiter == NULL) {
        return false;
    }
    const galay_c_async_waiter_impl_t* const impl = waiter->waiter;
    return atomic_load_explicit(&impl->ready, memory_order_acquire) != 0;
}

C_AsyncWaiterResultCode galay_c_async_waiter_notify(galay_c_async_waiter_t* waiter)
{
    if (waiter == NULL || waiter->waiter == NULL) {
        return C_AsyncWaiterParameterInvalid;
    }
    galay_c_async_waiter_impl_t* const impl = waiter->waiter;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&impl->ready, &expected, 1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return C_AsyncWaiterOperationInvalid;
    }
    C_CoroTaskInternal* const task = atomic_exchange_explicit(&impl->task, NULL,
                                                               memory_order_acq_rel);
    if (task != NULL) {
        const C_IOResult woke = galay_c_coro_task_wake(task);
        galay_c_coro_task_release(task);
        if (woke.code != C_IOResultOk) {
            return C_AsyncWaiterOperationInvalid;
        }
    }
    return C_AsyncWaiterSuccess;
}

static C_IOResult wait_with_deadline(galay_c_async_waiter_impl_t* impl,
                                     int64_t timeout_ms)
{
    const int64_t start = monotonic_milliseconds();
    if (start < 0) {
        return make_result(C_IOResultError, errno);
    }
    for (;;) {
        if (atomic_load_explicit(&impl->ready, memory_order_acquire) != 0) {
            return make_result(C_IOResultOk, 0);
        }
        if (timeout_ms == 0 || monotonic_milliseconds() - start >= timeout_ms) {
            return make_result(C_IOResultTimeout, 0);
        }
        const C_IOResult yielded = galay_c_coro_yield();
        if (yielded.code != C_IOResultOk) {
            return yielded;
        }
    }
}

C_IOResult galay_c_async_waiter_wait(galay_c_async_waiter_t* waiter,
                                          int64_t timeout_ms)
{
    if (waiter == NULL || waiter->waiter == NULL || timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    galay_c_async_waiter_impl_t* const impl = waiter->waiter;
    C_CoroTaskInternal* const current = galay_c_coro_task_current_internal();
    if (current == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    if (atomic_load_explicit(&impl->ready, memory_order_acquire) != 0) {
        return make_result(C_IOResultOk, 0);
    }
    if (timeout_ms >= 0) {
        return wait_with_deadline(impl, timeout_ms);
    }

    const C_IOResult prepared = galay_c_coro_task_prepare_wait();
    if (prepared.code != C_IOResultOk) {
        return prepared;
    }
    galay_c_coro_task_retain(current);
    C_CoroTaskInternal* expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&impl->task, &expected, current,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        galay_c_coro_task_release(current);
        const C_IOResult rolled_back = galay_c_coro_task_rollback_wait();
        return rolled_back.code == C_IOResultOk
            ? make_result(C_IOResultInvalid, 0)
            : rolled_back;
    }

    if (atomic_load_explicit(&impl->ready, memory_order_acquire) != 0) {
        expected = current;
        if (atomic_compare_exchange_strong_explicit(&impl->task, &expected, NULL,
                                                     memory_order_acq_rel,
                                                     memory_order_acquire)) {
            galay_c_coro_task_release(current);
            const C_IOResult rolled_back = galay_c_coro_task_rollback_wait();
            return rolled_back.code == C_IOResultOk
                ? make_result(C_IOResultOk, 0)
                : rolled_back;
        }
    }
    const C_IOResult parked = galay_c_coro_task_park_prepared();
    return parked.code == C_IOResultOk &&
                   atomic_load_explicit(&impl->ready, memory_order_acquire) != 0
        ? make_result(C_IOResultOk, 0)
        : parked;
}
