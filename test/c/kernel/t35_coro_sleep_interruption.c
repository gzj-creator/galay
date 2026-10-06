#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_sleep.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_internal.h>

#include <assert.h>
#include <stdint.h>

typedef struct SleepState {
    galay_c_runtime_t* runtime;
    C_IOResult sleep_result;
} SleepState;

static void sleep_entry(void* arg)
{
    SleepState* const state = arg;
    state->sleep_result = galay_c_coro_sleep(INT64_MAX);
}

static void interrupt_entry(void* arg)
{
    SleepState* const state = arg;
    galay_c_coro_task_t sleeper = {0};
    assert(galay_c_coro_spawn(state->runtime, sleep_entry, state, NULL, &sleeper).code ==
           C_IOResultOk);
    assert(galay_c_coro_yield().code == C_IOResultOk);
    assert(galay_c_coro_task_wake(sleeper.task).code == C_IOResultOk);
    assert(galay_c_coro_yield().code == C_IOResultOk);
    assert(state->sleep_result.code == C_IOResultCancelled);
    assert(galay_c_coro_destroy(&sleeper).code == C_IOResultOk);

    // Cancelling a queued wake and releasing its handle must also reclaim its deadline.
    assert(galay_c_coro_spawn(state->runtime, sleep_entry, state, NULL, &sleeper).code ==
           C_IOResultOk);
    assert(galay_c_coro_yield().code == C_IOResultOk);
    assert(galay_c_coro_task_wake(sleeper.task).code == C_IOResultOk);
    assert(galay_c_coro_cancel(&sleeper).code == C_IOResultCancelled);
    assert(galay_c_coro_destroy(&sleeper).code == C_IOResultOk);
    assert(galay_c_coro_yield().code == C_IOResultOk);
}

int main(void)
{
    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.parallel_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_coro_task_t task = {0};
    SleepState state = {.runtime = &runtime};
    assert(galay_c_runtime_create(&config, &runtime) == C_RuntimeSuccess);
    assert(galay_c_runtime_start(&runtime) == C_RuntimeSuccess);
    assert(galay_c_coro_spawn(&runtime, interrupt_entry, &state, NULL, &task).code ==
           C_IOResultOk);
    assert(galay_c_coro_join(&task, 5000).code == C_IOResultOk);
    assert(galay_c_coro_destroy(&task).code == C_IOResultOk);
    assert(galay_c_runtime_stop(&runtime) == C_RuntimeSuccess);
    assert(galay_c_runtime_destroy(&runtime) == C_RuntimeSuccess);
}
