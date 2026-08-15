#include "coro_wait.h"
#include "coro_task_internal.h"

#include <errno.h>
#include <sys/socket.h>

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

static C_IOResult make_result_bytes(C_IOResultCode code, size_t bytes, int sys_errno)
{
    return (C_IOResult){code, sys_errno, bytes, 0, NULL};
}

static C_IOResult clear_wait_slot(galay_c_io_controller_t* controller,
                                  uint32_t event_type,
                                  C_CoroTaskInternal* task)
{
    return event_type == GALAY_C_EVENT_READ
        ? galay_c_io_controller_clear_read(controller, task)
        : galay_c_io_controller_clear_write(controller, task);
}

C_IOResult galay_c_coro_wait_io(galay_c_io_scheduler_t* scheduler,
                                galay_c_io_controller_t* controller,
                                uint32_t event_type,
                                int64_t timeout_ms)
{
    if (scheduler == NULL || controller == NULL || timeout_ms < -1 ||
        (event_type != GALAY_C_EVENT_READ && event_type != GALAY_C_EVENT_WRITE)) {
        return make_result(C_IOResultInvalid, 0);
    }

    C_CoroTaskInternal* const current = galay_c_coro_task_current_internal();
    if (current == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    atomic_store_explicit(&current->wait_code, C_IOResultOk, memory_order_release);

    galay_c_coro_task_retain(current);
    C_IOResult registered = event_type == GALAY_C_EVENT_READ
        ? galay_c_io_controller_register_read(controller, current, scheduler)
        : galay_c_io_controller_register_write(controller, current, scheduler);
    if (registered.code != C_IOResultOk) {
        galay_c_coro_task_release(current);
        return registered;
    }

    const uint32_t current_events = atomic_load_explicit(&controller->registered_events,
                                                          memory_order_acquire);
    registered = galay_c_io_scheduler_register(scheduler, controller,
                                                current_events | event_type);
    if (registered.code != C_IOResultOk) {
        const C_IOResult cleared = clear_wait_slot(controller, event_type, current);
        if (cleared.code == C_IOResultOk) {
            galay_c_coro_task_release(current);
        }
        return registered;
    }
    if (timeout_ms == 0) {
        const C_IOResult cleared = clear_wait_slot(controller, event_type, current);
        if (cleared.code == C_IOResultOk) {
            galay_c_coro_task_release(current);
            return make_result(C_IOResultTimeout, 0);
        }
        const C_IOResultCode wait_code =
            atomic_load_explicit(&current->wait_code, memory_order_acquire);
        return wait_code == C_IOResultOk || wait_code == C_IOResultCancelled
            ? make_result(wait_code, 0)
            : make_result(C_IOResultError, 0);
    }
    if (timeout_ms > 0) {
        const C_IOResult timer_registered =
            galay_c_coro_task_register_timeout(current, controller, event_type, timeout_ms);
        if (timer_registered.code != C_IOResultOk) {
            if (clear_wait_slot(controller, event_type, current).code == C_IOResultOk) {
                galay_c_coro_task_release(current);
            }
            return timer_registered;
        }
    }

    const C_IOResult parked = galay_c_coro_task_suspend_current(C_CoroStateWaiting);
    galay_c_coro_task_cancel_timeout(current);
    if (parked.code != C_IOResultOk) {
        if (clear_wait_slot(controller, event_type, current).code == C_IOResultOk) {
            galay_c_coro_task_release(current);
        }
        return parked;
    }

    const int still_registered = event_type == GALAY_C_EVENT_READ
        ? galay_c_io_controller_has_read_coro(controller, current)
        : galay_c_io_controller_has_write_coro(controller, current);
    if (still_registered) {
        if (clear_wait_slot(controller, event_type, current).code == C_IOResultOk) {
            galay_c_coro_task_release(current);
        }
        return make_result(C_IOResultCancelled, 0);
    }
    const C_IOResultCode wait_code =
        atomic_load_explicit(&current->wait_code, memory_order_acquire);
    return wait_code == C_IOResultOk || wait_code == C_IOResultTimeout ||
                   wait_code == C_IOResultCancelled
        ? make_result(wait_code, 0)
        : make_result(C_IOResultError, 0);
}

C_IOResult galay_c_coro_cancel_io(galay_c_io_scheduler_t* scheduler,
                                  galay_c_io_controller_t* controller,
                                  uint32_t event_type)
{
    if (scheduler == NULL || controller == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }

    C_CoroTaskInternal* task = NULL;
    if (event_type == GALAY_C_EVENT_READ) {
        task = atomic_exchange_explicit(&controller->read_slot, NULL, memory_order_acq_rel);
    } else if (event_type == GALAY_C_EVENT_WRITE) {
        task = atomic_exchange_explicit(&controller->write_slot, NULL, memory_order_acq_rel);
    } else {
        return make_result(C_IOResultInvalid, 0);
    }
    if (task == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }

    task->result = make_result(C_IOResultCancelled, 0);
    atomic_store_explicit(&task->wait_code, C_IOResultCancelled, memory_order_release);
    const C_IOResult wake_result = galay_c_coro_task_wake(task);
    galay_c_coro_task_release(task);
    return wake_result.code == C_IOResultOk ? make_result(C_IOResultCancelled, 0) : wake_result;
}

C_IOResult galay_c_coro_recv_blocking(galay_c_io_scheduler_t* scheduler,
                                      galay_c_io_controller_t* controller,
                                      char* buffer,
                                      size_t length,
                                      int64_t timeout_ms)
{
    if (scheduler == NULL || controller == NULL || buffer == NULL || length == 0) {
        return make_result(C_IOResultInvalid, 0);
    }
    ssize_t received = recv(controller->fd, buffer, length, MSG_DONTWAIT);
    if (received > 0) {
        return make_result_bytes(C_IOResultOk, (size_t)received, 0);
    }
    if (received == 0) {
        return make_result(C_IOResultEof, 0);
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return make_result(C_IOResultError, errno);
    }

    const C_IOResult wait_result = galay_c_coro_wait_io(scheduler, controller,
                                                         GALAY_C_EVENT_READ, timeout_ms);
    if (wait_result.code != C_IOResultOk) {
        return wait_result;
    }
    received = recv(controller->fd, buffer, length, MSG_DONTWAIT);
    if (received > 0) {
        return make_result_bytes(C_IOResultOk, (size_t)received, 0);
    }
    if (received == 0) {
        return make_result(C_IOResultEof, 0);
    }
    return make_result(C_IOResultError, errno);
}

C_IOResult galay_c_coro_send_blocking(galay_c_io_scheduler_t* scheduler,
                                      galay_c_io_controller_t* controller,
                                      const char* buffer,
                                      size_t length,
                                      int64_t timeout_ms)
{
    if (scheduler == NULL || controller == NULL || buffer == NULL || length == 0) {
        return make_result(C_IOResultInvalid, 0);
    }
    ssize_t sent = send(controller->fd, buffer, length, MSG_DONTWAIT);
    if (sent > 0) {
        return make_result_bytes(C_IOResultOk, (size_t)sent, 0);
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return make_result(C_IOResultError, errno);
    }

    const C_IOResult wait_result = galay_c_coro_wait_io(scheduler, controller,
                                                         GALAY_C_EVENT_WRITE, timeout_ms);
    if (wait_result.code != C_IOResultOk) {
        return wait_result;
    }
    sent = send(controller->fd, buffer, length, MSG_DONTWAIT);
    if (sent > 0) {
        return make_result_bytes(C_IOResultOk, (size_t)sent, 0);
    }
    return make_result(C_IOResultError, errno);
}
