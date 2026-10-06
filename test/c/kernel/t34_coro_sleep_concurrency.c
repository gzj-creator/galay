#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_sleep.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>

enum { SLEEPER_COUNT = 64, SLEEP_ROUNDS = 3 };

typedef struct SleepState {
    _Atomic int* start;
    C_IOResult result;
    int rounds;
} SleepState;

static void sleep_entry(void* arg)
{
    SleepState* const state = arg;
    while (!atomic_load_explicit(state->start, memory_order_acquire)) {
        state->result = galay_c_coro_yield();
        if (state->result.code != C_IOResultOk) {
            return;
        }
    }
    for (int round = 0; round < SLEEP_ROUNDS; ++round) {
        state->result = galay_c_coro_sleep(100);
        if (state->result.code != C_IOResultOk) {
            return;
        }
        ++state->rounds;
    }
}

int main(void)
{
    struct rlimit original_limit;
    if (getrlimit(RLIMIT_NOFILE, &original_limit) != 0) {
        return 1;
    }
    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.parallel_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_coro_task_t tasks[SLEEPER_COUNT] = {{0}};
    SleepState states[SLEEPER_COUNT] = {{0}};
    _Atomic int start = 0;
    int result = 0;
    int spawned = 0;
    int limit_changed = 0;

    if (galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_c_runtime_start(&runtime) != C_RuntimeSuccess) {
        result = 2;
        goto cleanup;
    }
    // Sleeping tasks must not consume one OS descriptor per deadline.
    struct rlimit limited = original_limit;
    if (limited.rlim_cur > 32) {
        limited.rlim_cur = 32;
    }
    if (setrlimit(RLIMIT_NOFILE, &limited) != 0) {
        result = 3;
        goto cleanup;
    }
    limit_changed = 1;
    for (; spawned < SLEEPER_COUNT; ++spawned) {
        states[spawned].start = &start;
        const C_IOResult created = galay_c_coro_spawn(
            &runtime, sleep_entry, &states[spawned], NULL, &tasks[spawned]);
        if (created.code == C_IOResultError && created.sys_errno == ENOTSUP) {
            result = 125;
            break;
        }
        if (created.code != C_IOResultOk) {
            result = 4;
            break;
        }
    }

cleanup:
    atomic_store_explicit(&start, 1, memory_order_release);
    for (int i = 0; i < spawned; ++i) {
        const C_IOResult joined = galay_c_coro_join(&tasks[i], 5000);
        if (joined.code != C_IOResultOk && result == 0) {
            result = 5;
        }
        if (states[i].result.code != C_IOResultOk || states[i].rounds != SLEEP_ROUNDS) {
            fprintf(stderr, "sleeper %d: code=%d errno=%d rounds=%d\n", i,
                    states[i].result.code, states[i].result.sys_errno, states[i].rounds);
            if (result == 0) {
                result = 6;
            }
        }
        const C_IOResult destroyed = galay_c_coro_destroy(&tasks[i]);
        if (destroyed.code != C_IOResultOk && result == 0) {
            result = 7;
        }
    }
    if (limit_changed && setrlimit(RLIMIT_NOFILE, &original_limit) != 0 && result == 0) {
        result = 8;
    }
    if (runtime.runtime != NULL) {
        if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 9;
        }
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 10;
        }
    }
    return result;
}
