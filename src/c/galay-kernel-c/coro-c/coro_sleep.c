#include "coro_sleep.h"

#include "coro_task_internal.h"
#include "coro_wait.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/timerfd.h>
#endif

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

C_IOResult galay_c_coro_sleep(int64_t timeout_ms)
{
    if (timeout_ms < 0) {
        return make_result(C_IOResultInvalid, 0);
    }
    if (galay_c_coro_task_current_internal() == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    if (timeout_ms == 0) {
        return galay_c_coro_yield();
    }
#ifdef __linux__
    C_CoroTaskInternal* const current = galay_c_coro_task_current_internal();
    const int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) {
        return make_result(C_IOResultError, errno);
    }
    const struct itimerspec timer = {
        .it_interval = {0, 0},
        .it_value = {
            .tv_sec = timeout_ms / 1000,
            .tv_nsec = (timeout_ms % 1000) * 1000000,
        },
    };
    if (timerfd_settime(timer_fd, 0, &timer, NULL) != 0) {
        const int saved_errno = errno;
        const int close_result = close(timer_fd);
        return make_result(C_IOResultError,
                           close_result == 0 ? saved_errno : errno);
    }

    galay_c_io_controller_t controller;
    C_IOResult result = galay_c_io_controller_init(&controller, timer_fd, NULL);
    if (result.code == C_IOResultOk) {
        result = galay_c_coro_wait_io(current->owner, &controller, GALAY_C_EVENT_READ, -1);
    }
    if (result.code == C_IOResultOk) {
        uint64_t expirations = 0;
        const ssize_t bytes = read(timer_fd, &expirations, sizeof(expirations));
        if (bytes != (ssize_t)sizeof(expirations)) {
            result = make_result(C_IOResultError, bytes < 0 ? errno : EIO);
        }
    }
    if (atomic_load_explicit(&controller.registered_events, memory_order_acquire) !=
        GALAY_C_EVENT_NONE) {
        const C_IOResult unregistered =
            galay_c_io_scheduler_unregister(current->owner, &controller);
        if (result.code == C_IOResultOk && unregistered.code != C_IOResultOk) {
            result = unregistered;
        }
    }
    const C_IOResult cleaned = galay_c_io_controller_cleanup(&controller);
    if (result.code == C_IOResultOk && cleaned.code != C_IOResultOk) {
        result = cleaned;
    }
    if (close(timer_fd) != 0 && result.code == C_IOResultOk) {
        result = make_result(C_IOResultError, errno);
    }
    return result;
#else
    (void)timeout_ms;
    return make_result(C_IOResultError, ENOTSUP);
#endif
}
