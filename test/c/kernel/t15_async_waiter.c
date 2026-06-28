#include <galay/c/galay-kernel-c/concurrency-c/async_waiter_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <stdatomic.h>
#include <time.h>

typedef struct WaitState {
    galay_kernel_async_waiter_t* waiter;
    C_IOResult result;
    atomic_int phase;
    int64_t timeout_ms;
} WaitState;

static int expect_status(C_AsyncWaiterResultCode actual, C_AsyncWaiterResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_io_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static int create_started_runtime(galay_kernel_runtime_t* runtime)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    if (galay_kernel_runtime_create(&config, runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(runtime) != C_RuntimeSuccess) {
        if (galay_kernel_runtime_destroy(runtime) != C_RuntimeSuccess) {
            return 2;
        }
        return 1;
    }
    return 0;
}

static int wait_phase(atomic_int* phase, int expected)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(phase) == expected) {
            return 0;
        }
        if (nanosleep(&pause, 0) != 0 && errno != EINTR) {
            return 1;
        }
    }
    return 1;
}

static void wait_entry(void* ctx)
{
    WaitState* state = (WaitState*)ctx;
    state->result = galay_kernel_async_waiter_wait(state->waiter, state->timeout_ms);
    atomic_store(&state->phase, 1);
}

static int test_notify_before_wait(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_waiter_t waiter = {0};
    galay_coro_task_t task = {0};
    WaitState state;
    int exit_code = 0;

    atomic_init(&state.phase, 0);
    state.waiter = &waiter;
    state.timeout_ms = -1;

    if (create_started_runtime(&runtime) != 0) {
        return 1;
    }
    if (expect_status(galay_kernel_async_waiter_create(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 2;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_notify(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 3;
        goto cleanup;
    }
    if (!galay_kernel_async_waiter_is_ready(&waiter) ||
        galay_kernel_async_waiter_is_waiting(&waiter)) {
        exit_code = 4;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_spawn(&runtime, wait_entry, &state, 0, &task),
            C_IOResultOk) ||
        expect_io_code(galay_coro_join(&task, 2000), C_IOResultOk) ||
        state.result.code != C_IOResultOk) {
        exit_code = 5;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 6;
        }
    }
    if (waiter.waiter != 0) {
        if (galay_kernel_async_waiter_destroy(&waiter) != C_AsyncWaiterSuccess &&
            exit_code == 0) {
            exit_code = 7;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 8;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 9;
        }
    }
    return exit_code;
}

static int test_wait_before_notify(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_waiter_t waiter = {0};
    galay_coro_task_t task = {0};
    WaitState state;
    int exit_code = 0;

    atomic_init(&state.phase, 0);
    state.waiter = &waiter;
    state.timeout_ms = -1;

    if (create_started_runtime(&runtime) != 0) {
        return 10;
    }
    if (expect_status(galay_kernel_async_waiter_create(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 11;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_spawn(&runtime, wait_entry, &state, 0, &task),
            C_IOResultOk)) {
        exit_code = 12;
        goto cleanup;
    }
    for (int i = 0; i < 200; ++i) {
        if (galay_kernel_async_waiter_is_waiting(&waiter)) {
            break;
        }
        struct timespec pause = {0, 1000000};
        if (nanosleep(&pause, 0) != 0 && errno != EINTR) {
            exit_code = 13;
            goto cleanup;
        }
    }
    if (!galay_kernel_async_waiter_is_waiting(&waiter) ||
        galay_kernel_async_waiter_is_ready(&waiter)) {
        exit_code = 13;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_notify(&waiter), C_AsyncWaiterSuccess) ||
        wait_phase(&state.phase, 1) != 0 ||
        expect_io_code(galay_coro_join(&task, 2000), C_IOResultOk) ||
        state.result.code != C_IOResultOk ||
        !galay_kernel_async_waiter_is_ready(&waiter)) {
        exit_code = 14;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 15;
        }
    }
    if (waiter.waiter != 0) {
        if (galay_kernel_async_waiter_destroy(&waiter) != C_AsyncWaiterSuccess &&
            exit_code == 0) {
            exit_code = 16;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 17;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 18;
        }
    }
    return exit_code;
}

static int test_duplicate_notify(void)
{
    galay_kernel_async_waiter_t waiter = {0};
    int exit_code = 0;

    if (expect_status(galay_kernel_async_waiter_create(&waiter), C_AsyncWaiterSuccess)) {
        return 20;
    }
    if (expect_status(galay_kernel_async_waiter_notify(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 21;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_notify(&waiter), C_AsyncWaiterOperationInvalid)) {
        exit_code = 22;
        goto cleanup;
    }

cleanup:
    if (waiter.waiter != 0) {
        if (galay_kernel_async_waiter_destroy(&waiter) != C_AsyncWaiterSuccess &&
            exit_code == 0) {
            exit_code = 23;
        }
    }
    return exit_code;
}

static int test_timeout(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_waiter_t waiter = {0};
    galay_coro_task_t task = {0};
    WaitState state;
    int exit_code = 0;

    atomic_init(&state.phase, 0);
    state.waiter = &waiter;
    state.timeout_ms = 20;

    if (create_started_runtime(&runtime) != 0) {
        return 30;
    }
    if (expect_status(galay_kernel_async_waiter_create(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 31;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_spawn(&runtime, wait_entry, &state, 0, &task),
            C_IOResultOk) ||
        wait_phase(&state.phase, 1) != 0 ||
        expect_io_code(galay_coro_join(&task, 2000), C_IOResultOk) ||
        state.result.code != C_IOResultTimeout ||
        galay_kernel_async_waiter_is_ready(&waiter)) {
        exit_code = 32;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 33;
        }
    }
    if (waiter.waiter != 0) {
        if (galay_kernel_async_waiter_destroy(&waiter) != C_AsyncWaiterSuccess &&
            exit_code == 0) {
            exit_code = 34;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 35;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 36;
        }
    }
    return exit_code;
}

int main(void)
{
    galay_kernel_async_waiter_t invalid = {0};
    if (galay_kernel_async_waiter_get_error(C_AsyncWaiterSuccess) == 0 ||
        expect_status(galay_kernel_async_waiter_create(0), C_AsyncWaiterParameterInvalid) ||
        expect_status(galay_kernel_async_waiter_destroy(0), C_AsyncWaiterParameterInvalid) ||
        galay_kernel_async_waiter_is_waiting(0) ||
        galay_kernel_async_waiter_is_ready(0) ||
        expect_status(galay_kernel_async_waiter_notify(0), C_AsyncWaiterParameterInvalid) ||
        expect_io_code(galay_kernel_async_waiter_wait(0, -1), C_IOResultInvalid) ||
        expect_io_code(galay_kernel_async_waiter_wait(&invalid, -1), C_IOResultInvalid)) {
        return 1;
    }

    int result = test_notify_before_wait();
    if (result != 0) {
        return result;
    }
    result = test_wait_before_notify();
    if (result != 0) {
        return result;
    }
    result = test_duplicate_notify();
    if (result != 0) {
        return result;
    }
    return test_timeout();
}
