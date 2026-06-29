#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_wait_c.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>

enum {
    CORO_SLEEP_ITERATIONS = 64,
    CORO_SLEEP_MS = 1
};

typedef struct SleepBenchState {
    C_IOResult result;
    int64_t elapsed_us;
    int completed;
} SleepBenchState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return -1;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void sleep_bench_entry(void* ctx)
{
    SleepBenchState* state = (SleepBenchState*)ctx;
    const int64_t start = now_us();
    if (start < 0) {
        state->result = (C_IOResult){C_IOResultError, errno, 0, 0, 0};
        return;
    }

    for (int i = 0; i < CORO_SLEEP_ITERATIONS; ++i) {
        state->result = galay_coro_sleep(CORO_SLEEP_MS);
        if (state->result.code != C_IOResultOk) {
            return;
        }
        state->completed += 1;
    }

    const int64_t end = now_us();
    if (end < start) {
        state->result = (C_IOResult){C_IOResultError, 0, 0, 0, 0};
        return;
    }
    state->elapsed_us = end - start;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t task = {0};
    SleepBenchState state = {
        {C_IOResultError, 0, 0, 0, 0},
        0,
        0
    };
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    C_IOResult spawned = galay_coro_spawn(&runtime, sleep_bench_entry, &state, 0, &task);
    if (spawned.code == C_IOResultError && spawned.sys_errno == ENOTSUP) {
        if (printf("coro_sleep_latency unsupported C coroutine context\n") < 0) {
            exit_code = 3;
        }
        goto cleanup;
    }
    if (spawned.code != C_IOResultOk) {
        exit_code = 4;
        goto cleanup;
    }
    if (galay_coro_join(&task, 10000).code != C_IOResultOk) {
        exit_code = 5;
        goto cleanup;
    }
    if (galay_coro_destroy(&task).code != C_IOResultOk) {
        exit_code = 6;
        goto cleanup;
    }

    if (state.result.code != C_IOResultOk || state.completed != CORO_SLEEP_ITERATIONS) {
        exit_code = 7;
        goto cleanup;
    }

    {
        const double avg_us = state.completed > 0
            ? (double)state.elapsed_us / (double)state.completed
            : 0.0;
        if (printf("coro_sleep_latency iterations=%d sleep_ms=%d elapsed_ms=%.3f avg_us=%.3f\n",
                   state.completed,
                   CORO_SLEEP_MS,
                   (double)state.elapsed_us / 1000.0,
                   avg_us) < 0) {
            exit_code = 8;
        }
    }

cleanup:
    if (task.task != 0) {
        C_IOResult destroyed = galay_coro_destroy(&task);
        if (destroyed.code != C_IOResultOk && exit_code == 0) {
            exit_code = 9;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 10;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 11;
        }
    }
    return exit_code;
}
