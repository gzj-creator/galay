#include <galay/c/galay-kernel-c/concurrency-c/async_mutex_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

enum {
    ASYNC_MUTEX_ITERATIONS = 4096
};

typedef struct LockState {
    galay_kernel_async_mutex_t* mutex;
    C_IOResult lock_result;
    C_AsyncMutexResultCode unlock_result;
} LockState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void lock_entry(void* ctx)
{
    LockState* state = (LockState*)ctx;
    state->lock_result = galay_kernel_async_mutex_lock(state->mutex, 5000);
    if (state->lock_result.code == C_IOResultOk) {
        state->unlock_result = galay_kernel_async_mutex_unlock(state->mutex);
    }
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_mutex_t mutex = {0};
    galay_coro_task_t* tasks = 0;
    LockState* states = 0;
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_async_mutex_create(&mutex) != C_AsyncMutexSuccess) {
        return 1;
    }

    tasks = (galay_coro_task_t*)calloc((size_t)ASYNC_MUTEX_ITERATIONS, sizeof(galay_coro_task_t));
    states = (LockState*)calloc((size_t)ASYNC_MUTEX_ITERATIONS, sizeof(LockState));
    if (tasks == 0 || states == 0) {
        exit_code = 2;
        goto cleanup;
    }

    const int64_t start = now_us();
    for (int i = 0; i < ASYNC_MUTEX_ITERATIONS; ++i) {
        states[i].mutex = &mutex;
        states[i].unlock_result = C_AsyncMutexIOFailed;
        if (galay_coro_spawn(&runtime, lock_entry, &states[i], 0, &tasks[i]).code != C_IOResultOk) {
            exit_code = 3;
            goto cleanup;
        }
    }
    for (int i = 0; i < ASYNC_MUTEX_ITERATIONS; ++i) {
        if (galay_coro_join(&tasks[i], 8000).code != C_IOResultOk ||
            states[i].lock_result.code != C_IOResultOk ||
            states[i].unlock_result != C_AsyncMutexSuccess) {
            exit_code = 4;
            goto cleanup;
        }
    }
    const int64_t elapsed = now_us() - start;

    {
        const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
        const double ops_per_sec = seconds > 0.0 ? (double)ASYNC_MUTEX_ITERATIONS / seconds : 0.0;
        if (printf("async_mutex_contended iterations=%d elapsed_ms=%.3f ops_per_sec=%.2f\n",
                   ASYNC_MUTEX_ITERATIONS,
                   (double)elapsed / 1000.0,
                   ops_per_sec) < 0) {
            exit_code = 5;
            goto cleanup;
        }
    }

cleanup:
    if (tasks != 0) {
        for (int i = 0; i < ASYNC_MUTEX_ITERATIONS; ++i) {
            if (tasks[i].task != 0 &&
                galay_coro_destroy(&tasks[i]).code != C_IOResultOk &&
                exit_code == 0) {
                exit_code = 6;
            }
        }
    }
    if (mutex.mutex != 0) {
        if (galay_kernel_async_mutex_destroy(&mutex) != C_AsyncMutexSuccess && exit_code == 0) {
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
    free(states);
    free(tasks);
    return exit_code;
}
