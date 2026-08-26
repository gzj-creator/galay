#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include <stdint.h>

typedef struct YieldState {
    int* sequence;
    int* cursor;
    int first;
    int second;
} YieldState;

static void yielding_entry(void* arg)
{
    YieldState* state = (YieldState*)arg;
    state->sequence[(*state->cursor)++] = state->first;
    (void)galay_c_coro_yield();
    state->sequence[(*state->cursor)++] = state->second;
}

static void noop_entry(void* arg)
{
    (void)arg;
}

typedef struct CancelReadyState {
    galay_c_runtime_t* runtime;
    galay_c_coro_task_t child;
    C_IOResult spawn_result;
    C_IOResult cancel_result;
} CancelReadyState;

static void cancel_ready_entry(void* arg)
{
    CancelReadyState* state = (CancelReadyState*)arg;
    state->spawn_result = galay_c_coro_spawn(state->runtime,
                                            noop_entry,
                                            0,
                                            0,
                                            &state->child);
    if (state->spawn_result.code == C_IOResultOk) {
        state->cancel_result = galay_c_coro_cancel(&state->child);
    }
}

typedef struct CurrentState {
    int observed;
    int join_inside_invalid;
    int destroy_running_invalid;
    galay_c_coro_task_t current;
} CurrentState;

static void current_entry(void* arg)
{
    CurrentState* state = (CurrentState*)arg;
    galay_c_coro_task_t current = {0};
    C_IOResult result = galay_c_coro_current(&current);
    if (result.code == C_IOResultOk && current.task != 0) {
        state->observed = 1;
        state->current = current;
    }
    if (galay_c_coro_join(&current, 0).code == C_IOResultInvalid) {
        state->join_inside_invalid = 1;
    }
    if (galay_c_coro_destroy(&current).code == C_IOResultInvalid) {
        state->destroy_running_invalid = 1;
    }
}

static int expect_code(C_IOResult result, C_IOResultCode code)
{
    return result.code == code ? 0 : 1;
}

static int contains_once(const int* values, int count, int expected)
{
    int seen = 0;
    for (int i = 0; i < count; ++i) {
        if (values[i] == expected) {
            ++seen;
        }
    }
    return seen == 1;
}

int main(void)
{
    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.parallel_scheduler_count = 0;

    galay_c_runtime_t runtime = {0};
    if (galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_c_runtime_start(&runtime) != C_RuntimeSuccess) {
        (void)galay_c_runtime_destroy(&runtime);
        return 2;
    }

    int sequence[4] = {0, 0, 0, 0};
    int cursor = 0;
    YieldState first = {.sequence = sequence, .cursor = &cursor, .first = 1, .second = 3};
    YieldState second = {.sequence = sequence, .cursor = &cursor, .first = 2, .second = 4};
    C_CoroOptions options = galay_c_coro_options_default();
    options.stack_size = 64 * 1024;

    galay_c_coro_task_t task1 = {0};
    galay_c_coro_task_t task2 = {0};
    if (expect_code(galay_c_coro_spawn(&runtime, yielding_entry, &first, &options, &task1),
                    C_IOResultOk)) {
        return 3;
    }
    if (expect_code(galay_c_coro_spawn(&runtime, yielding_entry, &second, &options, &task2),
                    C_IOResultOk)) {
        return 4;
    }
    if (task1.task == 0 || task2.task == 0) {
        return 5;
    }
    if (expect_code(galay_c_coro_join(&task1, 1000), C_IOResultOk)) {
        return 6;
    }
    if (expect_code(galay_c_coro_join(&task2, 1000), C_IOResultOk)) {
        return 7;
    }
    if (cursor != 4 ||
        !contains_once(sequence, 4, 1) ||
        !contains_once(sequence, 4, 2) ||
        !contains_once(sequence, 4, 3) ||
        !contains_once(sequence, 4, 4)) {
        return 8;
    }

    CurrentState current_state = {0};
    galay_c_coro_task_t current_task = {0};
    if (expect_code(galay_c_coro_spawn(&runtime, current_entry, &current_state, 0, &current_task),
                    C_IOResultOk)) {
        return 9;
    }
    if (expect_code(galay_c_coro_join(&current_task, 1000), C_IOResultOk) ||
        current_state.observed != 1 ||
        current_state.current.task == 0 ||
        current_state.join_inside_invalid != 1 ||
        current_state.destroy_running_invalid != 1) {
        return 10;
    }
    if (expect_code(galay_c_coro_cancel(&current_task), C_IOResultOk)) {
        return 27;
    }

    CancelReadyState cancel_state = {.runtime = &runtime};
    galay_c_coro_task_t cancel_parent_task = {0};
    if (expect_code(galay_c_coro_spawn(&runtime,
                                     cancel_ready_entry,
                                     &cancel_state,
                                     &options,
                                     &cancel_parent_task),
                    C_IOResultOk) ||
        expect_code(galay_c_coro_join(&cancel_parent_task, 1000), C_IOResultOk) ||
        expect_code(cancel_state.spawn_result, C_IOResultOk)) {
        return 20;
    }
    if (expect_code(cancel_state.cancel_result, C_IOResultCancelled)) {
        return 21;
    }
    if (expect_code(galay_c_coro_join(&cancel_state.child, 0), C_IOResultCancelled)) {
        return 22;
    }
    if (expect_code(galay_c_coro_destroy(&cancel_state.child), C_IOResultOk) ||
        cancel_state.child.task != 0 ||
        expect_code(galay_c_coro_destroy(&cancel_parent_task), C_IOResultOk)) {
        return 23;
    }

    CurrentState after_cancel_state = {0};
    galay_c_coro_task_t after_cancel_task = {0};
    if (expect_code(galay_c_coro_spawn(&runtime, current_entry, &after_cancel_state, 0,
                                     &after_cancel_task), C_IOResultOk)) {
        return 24;
    }
    if (expect_code(galay_c_coro_join(&after_cancel_task, 1000), C_IOResultOk) ||
        after_cancel_state.observed != 1) {
        return 25;
    }

    if (expect_code(galay_c_coro_spawn(0, yielding_entry, &first, &options, &task1),
                    C_IOResultInvalid)) {
        return 11;
    }
    if (expect_code(galay_c_coro_spawn(&runtime, 0, &first, &options, &task1),
                    C_IOResultInvalid)) {
        return 12;
    }
    if (expect_code(galay_c_coro_join(0, 0), C_IOResultInvalid)) {
        return 13;
    }
    if (expect_code(galay_c_coro_yield(), C_IOResultInvalid)) {
        return 14;
    }

    if (expect_code(galay_c_coro_destroy(&task1), C_IOResultOk)) {
        return 15;
    }
    if (expect_code(galay_c_coro_destroy(&task2), C_IOResultOk)) {
        return 16;
    }
    if (expect_code(galay_c_coro_destroy(&current_task), C_IOResultOk)) {
        return 17;
    }
    if (expect_code(galay_c_coro_destroy(&current_state.current), C_IOResultOk)) {
        return 28;
    }
    if (expect_code(galay_c_coro_destroy(&after_cancel_task), C_IOResultOk)) {
        return 26;
    }
    if (after_cancel_state.current.task != 0 &&
        expect_code(galay_c_coro_destroy(&after_cancel_state.current), C_IOResultOk)) {
        return 29;
    }
    if (task1.task != 0 || task2.task != 0 || current_task.task != 0 ||
        current_state.current.task != 0 || cancel_state.child.task != 0 ||
        cancel_parent_task.task != 0 ||
        after_cancel_task.task != 0 || after_cancel_state.current.task != 0) {
        return 18;
    }

    (void)galay_c_runtime_stop(&runtime);
    if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess) {
        return 19;
    }
    return 0;
}
