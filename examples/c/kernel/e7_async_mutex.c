#include <galay/c/galay-kernel-c/concurrency-c/async_mutex_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>

typedef struct LockState {
    galay_kernel_async_mutex_t* mutex;
    C_IOResult lock_result;
    C_AsyncMutexResultCode unlock_result;
    int id;
} LockState;

static void lock_entry(void* ctx)
{
    LockState* state = (LockState*)ctx;
    state->lock_result = galay_kernel_async_mutex_lock(state->mutex, 2000);
    if (state->lock_result.code != C_IOResultOk) {
        return;
    }
    if (printf("task %d entered critical section\n", state->id) < 0) {
        state->unlock_result = C_AsyncMutexIOFailed;
        return;
    }
    state->unlock_result = galay_kernel_async_mutex_unlock(state->mutex);
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
    int exit_code = 0;

    LockState first = {&mutex, {0}, C_AsyncMutexIOFailed, 1};
    LockState second = {&mutex, {0}, C_AsyncMutexIOFailed, 2};

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_async_mutex_create(&mutex) != C_AsyncMutexSuccess) {
        return 1;
    }

    if (galay_coro_spawn(&runtime, lock_entry, &first, 0, &first_task).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, lock_entry, &second, 0, &second_task).code != C_IOResultOk ||
        galay_coro_join(&first_task, 3000).code != C_IOResultOk ||
        galay_coro_join(&second_task, 3000).code != C_IOResultOk ||
        first.lock_result.code != C_IOResultOk ||
        first.unlock_result != C_AsyncMutexSuccess ||
        second.lock_result.code != C_IOResultOk ||
        second.unlock_result != C_AsyncMutexSuccess ||
        galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 2;
        goto cleanup;
    }

cleanup:
    if (second_task.task != 0) {
        if (galay_coro_destroy(&second_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 3;
        }
    }
    if (first_task.task != 0) {
        if (galay_coro_destroy(&first_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 4;
        }
    }
    if (mutex.mutex != 0) {
        if (galay_kernel_async_mutex_destroy(&mutex) != C_AsyncMutexSuccess && exit_code == 0) {
            exit_code = 5;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 6;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 7;
        }
    }
    return exit_code;
}
