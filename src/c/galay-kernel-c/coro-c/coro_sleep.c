#include "coro_sleep.h"

#include "coro_task_internal.h"
#include "../core-c/io_controller.h"

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

C_IOResult galay_c_coro_sleep(int64_t timeout_ms)
{
    if (timeout_ms < 0) {
        return make_result(C_IOResultInvalid, 0);
    }
    C_CoroTaskInternal* const current = galay_c_coro_task_current_internal();
    if (current == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    if (timeout_ms == 0) {
        return galay_c_coro_yield();
    }
    atomic_store_explicit(&current->wait_code, C_IOResultOk, memory_order_release);
    const C_IOResult registered = galay_c_coro_task_register_timeout(
        current, NULL, GALAY_C_EVENT_NONE, timeout_ms);
    if (registered.code != C_IOResultOk) {
        return registered;
    }
    const C_IOResult parked = galay_c_coro_task_suspend_current(C_CoroStateWaiting);
    const int interrupted = current->timeout_active;
    galay_c_coro_task_cancel_timeout(current);
    if (parked.code != C_IOResultOk) {
        return parked;
    }
    return make_result(interrupted ? C_IOResultCancelled :
        atomic_load_explicit(&current->wait_code, memory_order_acquire), 0);
}
