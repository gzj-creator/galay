#include <galay/c/galay-kernel-c/concurrency-c/async_mutex_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <stdatomic.h>
#include <time.h>

typedef struct LockState {
    galay_kernel_async_mutex_t* mutex;
    C_IOResult result;
    atomic_int phase;
} LockState;

enum {
    HANDOFF_STRESS_ITERATIONS = 512
};

static int expect_status(C_AsyncMutexResultCode actual, C_AsyncMutexResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_io_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static int wait_phase_for(atomic_int* phase, int expected, int attempts)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < attempts; ++i) {
        if (atomic_load(phase) == expected) {
            return 0;
        }
        if (nanosleep(&pause, 0) != 0 && errno != EINTR) {
            return 1;
        }
    }
    return 1;
}

static int wait_phase(atomic_int* phase, int expected)
{
    return wait_phase_for(phase, expected, 2000);
}

static void lock_entry(void* ctx)
{
    LockState* state = (LockState*)ctx;
    state->result = galay_kernel_async_mutex_lock(state->mutex, -1);
    if (state->result.code == C_IOResultOk) {
        atomic_store(&state->phase, 1);
    }
}

static void lock_timeout_entry(void* ctx)
{
    LockState* state = (LockState*)ctx;
    state->result = galay_kernel_async_mutex_lock(state->mutex, 20);
    atomic_store(&state->phase, 1);
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_mutex_t mutex = {0};
    galay_coro_task_t first_task = {0};
    galay_coro_task_t second_task = {0};
    LockState first;
    LockState second;
    int exit_code = 0;

    atomic_init(&first.phase, 0);
    atomic_init(&second.phase, 0);
    first.mutex = &mutex;
    second.mutex = &mutex;

    if (galay_kernel_async_mutex_get_error(C_AsyncMutexSuccess) == 0) {
        return 1;
    }
    if (expect_status(galay_kernel_async_mutex_create(0), C_AsyncMutexParameterInvalid)) {
        return 2;
    }
    if (expect_status(galay_kernel_async_mutex_destroy(0), C_AsyncMutexParameterInvalid)) {
        return 3;
    }
    if (galay_kernel_async_mutex_is_locked(0)) {
        return 4;
    }
    if (expect_status(galay_kernel_async_mutex_destroy(&mutex), C_AsyncMutexSuccess)) {
        return 5;
    }
    if (expect_status(galay_kernel_async_mutex_create(&mutex), C_AsyncMutexSuccess)) {
        return 6;
    }
    if (mutex.mutex == 0 || galay_kernel_async_mutex_is_locked(&mutex)) {
        return 7;
    }
    if (expect_io_code(galay_kernel_async_mutex_lock(0, -1), C_IOResultInvalid) ||
        expect_io_code(galay_kernel_async_mutex_lock(&mutex, -1), C_IOResultInvalid)) {
        return 8;
    }

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        exit_code = 9;
        goto cleanup;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 10;
        goto cleanup;
    }

    if (expect_io_code(galay_coro_spawn(&runtime, lock_entry, &first, 0, &first_task),
            C_IOResultOk) ||
        wait_phase(&first.phase, 1) != 0 ||
        first.result.code != C_IOResultOk ||
        !galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 11;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_mutex_unlock(&mutex), C_AsyncMutexSuccess) ||
        expect_io_code(galay_coro_join(&first_task, 2000), C_IOResultOk) ||
        galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 12;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_destroy(&first_task), C_IOResultOk)) {
        exit_code = 19;
        goto cleanup;
    }

    atomic_store(&first.phase, 0);
    atomic_store(&second.phase, 0);
    first.result.code = C_IOResultError;
    second.result.code = C_IOResultError;
    if (expect_io_code(galay_coro_spawn(&runtime, lock_entry, &first, 0, &first_task),
            C_IOResultOk) ||
        wait_phase(&first.phase, 1) != 0 ||
        expect_io_code(galay_coro_spawn(&runtime, lock_entry, &second, 0, &second_task),
            C_IOResultOk)) {
        exit_code = 13;
        goto cleanup;
    }
    if (wait_phase(&second.phase, 1) == 0) {
        exit_code = 14;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_mutex_unlock(&mutex), C_AsyncMutexSuccess) ||
        wait_phase(&second.phase, 1) != 0 ||
        second.result.code != C_IOResultOk ||
        !galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 15;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_mutex_unlock(&mutex), C_AsyncMutexSuccess) ||
        expect_io_code(galay_coro_join(&first_task, 2000), C_IOResultOk) ||
        expect_io_code(galay_coro_join(&second_task, 2000), C_IOResultOk) ||
        galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 16;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_destroy(&first_task), C_IOResultOk) ||
        expect_io_code(galay_coro_destroy(&second_task), C_IOResultOk)) {
        exit_code = 20;
        goto cleanup;
    }

    for (int i = 0; i < HANDOFF_STRESS_ITERATIONS; ++i) {
        atomic_store(&first.phase, 0);
        atomic_store(&second.phase, 0);
        first.result.code = C_IOResultError;
        second.result.code = C_IOResultError;
        if (expect_io_code(galay_coro_spawn(&runtime, lock_entry, &first, 0, &first_task),
                C_IOResultOk) ||
            wait_phase(&first.phase, 1) != 0 ||
            expect_io_code(galay_coro_spawn(&runtime, lock_entry, &second, 0, &second_task),
                C_IOResultOk)) {
            exit_code = 27;
            goto cleanup;
        }
        if (wait_phase_for(&second.phase, 1, 10) == 0) {
            exit_code = 28;
            goto cleanup;
        }
        if (expect_status(galay_kernel_async_mutex_unlock(&mutex), C_AsyncMutexSuccess) ||
            wait_phase(&second.phase, 1) != 0 ||
            second.result.code != C_IOResultOk ||
            !galay_kernel_async_mutex_is_locked(&mutex)) {
            exit_code = 29;
            goto cleanup;
        }
        if (expect_status(galay_kernel_async_mutex_unlock(&mutex), C_AsyncMutexSuccess) ||
            expect_io_code(galay_coro_join(&first_task, 2000), C_IOResultOk) ||
            expect_io_code(galay_coro_join(&second_task, 2000), C_IOResultOk) ||
            galay_kernel_async_mutex_is_locked(&mutex)) {
            exit_code = 30;
            goto cleanup;
        }
        if (expect_io_code(galay_coro_destroy(&first_task), C_IOResultOk) ||
            expect_io_code(galay_coro_destroy(&second_task), C_IOResultOk)) {
            exit_code = 31;
            goto cleanup;
        }
    }

    atomic_store(&first.phase, 0);
    atomic_store(&second.phase, 0);
    first.result.code = C_IOResultError;
    second.result.code = C_IOResultError;
    if (expect_io_code(galay_coro_spawn(&runtime, lock_entry, &first, 0, &first_task),
            C_IOResultOk) ||
        wait_phase(&first.phase, 1) != 0 ||
        expect_io_code(galay_coro_spawn(&runtime, lock_timeout_entry, &second, 0, &second_task),
            C_IOResultOk) ||
        wait_phase(&second.phase, 1) != 0 ||
        second.result.code != C_IOResultTimeout ||
        !galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 17;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_mutex_unlock(&mutex), C_AsyncMutexSuccess) ||
        expect_io_code(galay_coro_join(&first_task, 2000), C_IOResultOk) ||
        expect_io_code(galay_coro_join(&second_task, 2000), C_IOResultOk) ||
        galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 18;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_destroy(&first_task), C_IOResultOk) ||
        expect_io_code(galay_coro_destroy(&second_task), C_IOResultOk)) {
        exit_code = 21;
        goto cleanup;
    }

cleanup:
    if (first_task.task != 0) {
        if (galay_coro_destroy(&first_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 22;
        }
    }
    if (second_task.task != 0) {
        if (galay_coro_destroy(&second_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 23;
        }
    }
    if (mutex.mutex != 0) {
        if (galay_kernel_async_mutex_destroy(&mutex) != C_AsyncMutexSuccess &&
            exit_code == 0) {
            exit_code = 24;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 25;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 26;
        }
    }
    return exit_code;
}
