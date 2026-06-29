#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_wait_c.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>

typedef struct SleepState {
    C_IOResult zero_result;
    C_IOResult short_result;
    C_IOResult invalid_result;
    int64_t zero_elapsed_us;
    int64_t short_elapsed_us;
} SleepState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return -1;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void sleep_entry(void* ctx)
{
    SleepState* state = (SleepState*)ctx;

    const int64_t zero_start = now_us();
    state->zero_result = galay_coro_sleep(0);
    const int64_t zero_end = now_us();
    state->zero_elapsed_us = zero_start >= 0 && zero_end >= zero_start
        ? zero_end - zero_start
        : -1;

    const int64_t short_start = now_us();
    state->short_result = galay_coro_sleep(20);
    const int64_t short_end = now_us();
    state->short_elapsed_us = short_start >= 0 && short_end >= short_start
        ? short_end - short_start
        : -1;

    state->invalid_result = galay_coro_sleep(-1);
}

int main(void)
{
    if (galay_coro_sleep(0).code != C_IOResultInvalid) {
        return 1;
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t task = {0};
    SleepState state = {
        {C_IOResultError, 0, 0, 0, 0},
        {C_IOResultError, 0, 0, 0, 0},
        {C_IOResultError, 0, 0, 0, 0},
        -1,
        -1
    };
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 2;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 3;
        goto cleanup;
    }
    C_IOResult spawned = galay_coro_spawn(&runtime, sleep_entry, &state, 0, &task);
    if (spawned.code == C_IOResultError && spawned.sys_errno == ENOTSUP) {
        printf("coro_sleep skipped: C coroutine context unsupported\n");
        goto cleanup;
    }
    if (spawned.code != C_IOResultOk) {
        exit_code = 4;
        goto cleanup;
    }
    if (galay_coro_join(&task, 5000).code != C_IOResultOk) {
        exit_code = 5;
        goto cleanup;
    }
    if (galay_coro_destroy(&task).code != C_IOResultOk) {
        exit_code = 6;
        goto cleanup;
    }

    if (state.zero_result.code != C_IOResultOk ||
        state.short_result.code != C_IOResultOk ||
        state.invalid_result.code != C_IOResultInvalid) {
        exit_code = 7;
        goto cleanup;
    }
    if (state.zero_elapsed_us < 0 || state.zero_elapsed_us > 5000) {
        exit_code = 8;
        goto cleanup;
    }
    if (state.short_elapsed_us < 10000) {
        exit_code = 9;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        C_IOResult destroyed = galay_coro_destroy(&task);
        if (destroyed.code != C_IOResultOk && exit_code == 0) {
            exit_code = 10;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 11;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 12;
        }
    }
    return exit_code;
}
