#include <galay/c/galay-kernel-c/concurrency-c/async_waiter_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

enum {
    ASYNC_WAITER_ITERATIONS = 1000
};

typedef struct WaitState {
    galay_kernel_async_waiter_t* waiter;
    C_IOResult result;
} WaitState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void wait_entry(void* ctx)
{
    WaitState* state = (WaitState*)ctx;
    state->result = galay_kernel_async_waiter_wait(state->waiter, 2000);
}

int main(int argc, char** argv)
{
    int iterations = ASYNC_WAITER_ITERATIONS;
    if (argc == 2) {
        char* end = 0;
        errno = 0;
        long parsed = strtol(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0' || parsed <= 0 || parsed > INT_MAX) {
            fprintf(stderr, "usage: %s [positive-iterations]\n", argv[0]);
            return 1;
        }
        iterations = (int)parsed;
    } else if (argc > 2) {
        fprintf(stderr, "usage: %s [positive-iterations]\n", argv[0]);
        return 1;
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        return 1;
    }

    const int64_t start = now_us();
    for (int i = 0; i < iterations; ++i) {
        galay_kernel_async_waiter_t waiter = {0};
        galay_coro_task_t task = {0};
        WaitState state = {&waiter, {0}};

        if (galay_kernel_async_waiter_create(&waiter) != C_AsyncWaiterSuccess ||
            galay_coro_spawn(&runtime, wait_entry, &state, 0, &task).code != C_IOResultOk ||
            galay_kernel_async_waiter_notify(&waiter) != C_AsyncWaiterSuccess ||
            galay_coro_join(&task, 3000).code != C_IOResultOk ||
            state.result.code != C_IOResultOk) {
            exit_code = 2;
        }
        if (task.task != 0 &&
            galay_coro_destroy(&task).code != C_IOResultOk &&
            exit_code == 0) {
            exit_code = 3;
        }
        if (waiter.waiter != 0 &&
            galay_kernel_async_waiter_destroy(&waiter) != C_AsyncWaiterSuccess &&
            exit_code == 0) {
            exit_code = 4;
        }
        if (exit_code != 0) {
            break;
        }
    }

    const int64_t elapsed = now_us() - start;
    if (exit_code == 0) {
        const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
        const double ops_per_sec = seconds > 0.0 ? (double)iterations / seconds : 0.0;
        if (printf("async_waiter_signal iterations=%d elapsed_ms=%.3f ops_per_sec=%.2f\n",
                   iterations,
                   (double)elapsed / 1000.0,
                   ops_per_sec) < 0) {
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
