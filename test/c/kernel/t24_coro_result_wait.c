#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_wait_c.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
typedef struct WaitEntryState {
    C_CoroWaitRequest* request;
    C_IOResult result;
    atomic_int phase;
    int64_t timeout_ms;
} WaitEntryState;

typedef enum WaitDriverAction {
    WaitDriverComplete,
    WaitDriverCancel,
} WaitDriverAction;

typedef struct WaitDriverState {
    C_CoroWaitRequest* request;
    C_CoroWaitEventToken* token;
    void* user_data;
    WaitEntryState* waiter;
    uint64_t generation;
    C_IOResult result;
    C_IOResult destroy_result;
    C_IOResult finish_result;
    C_IOResult cancel_after_finish_result;
    galay_coro_task_t* cancel_after_finish_task;
    WaitDriverAction action;
    int use_token;
    int use_user_data;
} WaitDriverState;

static int expect_code(C_IOResult result, C_IOResultCode code)
{
    return result.code == code ? 0 : 1;
}

static void waiting_entry(void* arg)
{
    WaitEntryState* state = (WaitEntryState*)arg;
    atomic_store_explicit(&state->phase, 1, memory_order_release);
    state->result = galay_coro_wait(state->request, state->timeout_ms);
    atomic_store_explicit(&state->phase, 2, memory_order_release);
}

static void wait_driver_entry(void* arg)
{
    WaitDriverState* state = (WaitDriverState*)arg;
    for (;;) {
        if (atomic_load_explicit(&state->waiter->phase, memory_order_acquire) >= 1) {
            break;
        }
        (void)galay_coro_yield();
    }
    (void)galay_coro_yield();
    state->destroy_result = galay_coro_wait_request_destroy(state->request);
    if (state->action == WaitDriverComplete) {
        state->finish_result = state->use_user_data
            ? galay_coro_wait_event_user_data_complete(state->user_data, state->result)
            : state->use_token
            ? galay_coro_wait_event_token_complete(state->token, state->result)
            : galay_coro_wait_request_complete(state->request, state->generation, state->result);
    } else {
        state->finish_result = state->use_user_data
            ? galay_coro_wait_event_user_data_cancel(state->user_data)
            : state->use_token
            ? galay_coro_wait_event_token_cancel(state->token)
            : galay_coro_wait_request_cancel(state->request, state->generation);
    }
    if (state->cancel_after_finish_task != 0) {
        state->cancel_after_finish_result = galay_coro_cancel(state->cancel_after_finish_task);
    }
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        (void)galay_kernel_runtime_destroy(&runtime);
        return 2;
    }

    C_CoroWaitRequest request = {0};
    if (expect_code(galay_coro_wait_request_create(&request), C_IOResultOk)) {
        return 3;
    }
    if (expect_code(galay_coro_wait(&request, 0), C_IOResultInvalid)) {
        return 21;
    }

    uint64_t generation = 0;
    C_IOResult ready_result = {
        .code = C_IOResultOk,
        .sys_errno = 0,
        .bytes = 7,
        .value = 11,
        .ptr = 0,
    };
    if (expect_code(galay_coro_wait_request_prepare(&request, &generation), C_IOResultOk) ||
        expect_code(galay_coro_wait_request_complete(&request, generation, ready_result), C_IOResultOk)) {
        return 4;
    }
    if (expect_code(galay_coro_wait_request_complete(&request, generation, ready_result), C_IOResultInvalid)) {
        return 22;
    }

    C_CoroWaitEventToken token = {0};
    if (expect_code(galay_coro_wait_request_event_token_acquire(&request, generation, &token),
                    C_IOResultInvalid)) {
        return 25;
    }

    WaitEntryState immediate = {
        .request = &request,
        .phase = ATOMIC_VAR_INIT(0),
        .timeout_ms = 1000,
    };
    galay_coro_task_t immediate_task = {0};
    if (expect_code(galay_coro_spawn(&runtime, waiting_entry, &immediate, 0, &immediate_task),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&immediate_task, 1000), C_IOResultOk) ||
        immediate.result.code != C_IOResultOk ||
        immediate.result.bytes != 7 ||
        immediate.result.value != 11) {
        return 5;
    }
    if (expect_code(galay_coro_destroy(&immediate_task), C_IOResultOk)) {
        return 6;
    }

    if (expect_code(galay_coro_wait_request_prepare(&request, &generation), C_IOResultOk)) {
        return 36;
    }
    WaitEntryState huge_timeout = {
        .request = &request,
        .phase = ATOMIC_VAR_INIT(0),
        .timeout_ms = LLONG_MAX,
    };
    galay_coro_task_t huge_timeout_task = {0};
    if (expect_code(galay_coro_spawn(&runtime,
                                     waiting_entry,
                                     &huge_timeout,
                                     0,
                                     &huge_timeout_task),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&huge_timeout_task, 1000), C_IOResultOk) ||
        huge_timeout.result.code != C_IOResultInvalid ||
        expect_code(galay_coro_destroy(&huge_timeout_task), C_IOResultOk) ||
        expect_code(galay_coro_wait_request_cancel(&request, generation), C_IOResultCancelled) ||
        expect_code(galay_coro_wait_request_destroy(&request), C_IOResultOk) ||
        expect_code(galay_coro_wait_request_create(&request), C_IOResultOk)) {
        return 37;
    }

    if (expect_code(galay_coro_wait_request_prepare(&request, &generation), C_IOResultOk) ||
        expect_code(galay_coro_wait_request_destroy(&request), C_IOResultInvalid) ||
        expect_code(galay_coro_wait_request_complete(&request, generation + 1, ready_result), C_IOResultInvalid) ||
        expect_code(galay_coro_wait_request_cancel(&request, generation), C_IOResultCancelled)) {
        return 23;
    }
    if (expect_code(galay_coro_wait_request_destroy(&request), C_IOResultOk) ||
        expect_code(galay_coro_wait_request_create(&request), C_IOResultOk)) {
        return 27;
    }

    if (expect_code(galay_coro_wait_request_prepare(&request, &generation), C_IOResultOk) ||
        expect_code(galay_coro_wait_request_event_token_acquire(&request, generation, &token),
                    C_IOResultOk) ||
        expect_code(galay_coro_wait_request_complete(&request, generation, ready_result),
                    C_IOResultOk) ||
        expect_code(galay_coro_wait_request_destroy(&request), C_IOResultOk) ||
        expect_code(galay_coro_wait_event_token_complete(&token, ready_result),
                    C_IOResultInvalid) ||
        expect_code(galay_coro_wait_event_token_release(&token), C_IOResultOk)) {
        return 26;
    }
    if (expect_code(galay_coro_wait_request_create(&request), C_IOResultOk)) {
        return 28;
    }

    if (expect_code(galay_coro_wait_request_prepare(&request, &generation), C_IOResultOk) ||
        expect_code(galay_coro_wait_request_event_token_acquire(&request, generation, &token),
                    C_IOResultOk) ||
        expect_code(galay_coro_wait_event_token_cancel(&token), C_IOResultCancelled) ||
        expect_code(galay_coro_wait_request_complete(&request, generation, ready_result),
                    C_IOResultInvalid) ||
        expect_code(galay_coro_wait_event_token_release(&token), C_IOResultOk) ||
        expect_code(galay_coro_wait_request_destroy(&request), C_IOResultOk) ||
        expect_code(galay_coro_wait_request_create(&request), C_IOResultOk)) {
        return 29;
    }

    if (expect_code(galay_coro_wait_request_prepare(&request, &generation), C_IOResultOk)) {
        return 7;
    }
    WaitEntryState suspended = {
        .request = &request,
        .phase = ATOMIC_VAR_INIT(0),
        .timeout_ms = 1000,
    };
    galay_coro_task_t suspended_task = {0};
    WaitDriverState suspended_driver = {
        .request = &request,
        .waiter = &suspended,
        .generation = generation,
        .action = WaitDriverComplete,
    };
    galay_coro_task_t suspended_driver_task = {0};
    if (expect_code(galay_coro_spawn(&runtime, waiting_entry, &suspended, 0, &suspended_task),
                    C_IOResultOk)) {
        return 8;
    }
    C_IOResult ok_result = {
        .code = C_IOResultOk,
        .sys_errno = 0,
        .bytes = 19,
        .value = 23,
        .ptr = 0,
    };
    suspended_driver.result = ok_result;
    suspended_driver.cancel_after_finish_task = &suspended_task;
    if (expect_code(galay_coro_spawn(&runtime,
                                     wait_driver_entry,
                                     &suspended_driver,
                                     0,
                                     &suspended_driver_task),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&suspended_driver_task, 1000), C_IOResultOk) ||
        expect_code(suspended_driver.destroy_result, C_IOResultInvalid) ||
        expect_code(suspended_driver.finish_result, C_IOResultOk) ||
        expect_code(suspended_driver.cancel_after_finish_result, C_IOResultInvalid) ||
        expect_code(galay_coro_join(&suspended_task, 1000), C_IOResultOk) ||
        suspended.result.code != C_IOResultOk ||
        suspended.result.bytes != 19 ||
        suspended.result.value != 23) {
        return 9;
    }
    if (expect_code(galay_coro_destroy(&suspended_driver_task), C_IOResultOk) ||
        expect_code(galay_coro_destroy(&suspended_task), C_IOResultOk)) {
        return 10;
    }

    if (expect_code(galay_coro_wait_request_prepare(&request, &generation), C_IOResultOk) ||
        expect_code(galay_coro_wait_request_event_token_acquire(&request, generation, &token),
                    C_IOResultOk)) {
        return 30;
    }
    void* token_user_data = 0;
    if (expect_code(galay_coro_wait_event_token_detach_user_data(&token, &token_user_data),
                    C_IOResultOk) ||
        expect_code(galay_coro_wait_event_token_release(&token), C_IOResultInvalid)) {
        return 38;
    }
    WaitEntryState token_complete = {
        .request = &request,
        .phase = ATOMIC_VAR_INIT(0),
        .timeout_ms = 1000,
    };
    galay_coro_task_t token_complete_task = {0};
    WaitDriverState token_complete_driver = {
        .request = &request,
        .user_data = token_user_data,
        .waiter = &token_complete,
        .generation = generation,
        .result = ok_result,
        .action = WaitDriverComplete,
        .use_user_data = 1,
    };
    galay_coro_task_t token_complete_driver_task = {0};
    if (expect_code(galay_coro_spawn(&runtime,
                                     waiting_entry,
                                     &token_complete,
                                     0,
                                     &token_complete_task),
                    C_IOResultOk) ||
        expect_code(galay_coro_spawn(&runtime,
                                     wait_driver_entry,
                                     &token_complete_driver,
                                     0,
                                     &token_complete_driver_task),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&token_complete_driver_task, 1000), C_IOResultOk) ||
        expect_code(token_complete_driver.destroy_result, C_IOResultInvalid) ||
        expect_code(token_complete_driver.finish_result, C_IOResultOk) ||
        expect_code(galay_coro_join(&token_complete_task, 1000), C_IOResultOk) ||
        token_complete.result.code != C_IOResultOk ||
        token_complete.result.bytes != 19 ||
        token_complete.result.value != 23 ||
        expect_code(galay_coro_wait_event_user_data_release(token_user_data), C_IOResultOk)) {
        return 31;
    }
    if (expect_code(galay_coro_destroy(&token_complete_driver_task), C_IOResultOk) ||
        expect_code(galay_coro_destroy(&token_complete_task), C_IOResultOk)) {
        return 32;
    }

    if (expect_code(galay_coro_wait_request_prepare(&request, &generation), C_IOResultOk)) {
        return 11;
    }
    WaitEntryState timeout_state = {
        .request = &request,
        .phase = ATOMIC_VAR_INIT(0),
        .timeout_ms = 5,
    };
    galay_coro_task_t timeout_task = {0};
    if (expect_code(galay_coro_spawn(&runtime, waiting_entry, &timeout_state, 0, &timeout_task),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&timeout_task, 1000), C_IOResultOk) ||
        timeout_state.result.code != C_IOResultTimeout) {
        return 12;
    }
    if (expect_code(galay_coro_wait_request_complete(&request, generation, ok_result), C_IOResultInvalid)) {
        return 13;
    }
    if (expect_code(galay_coro_destroy(&timeout_task), C_IOResultOk)) {
        return 14;
    }

    if (expect_code(galay_coro_wait_request_prepare(&request, &generation), C_IOResultOk)) {
        return 15;
    }
    WaitEntryState cancel_state = {
        .request = &request,
        .phase = ATOMIC_VAR_INIT(0),
        .timeout_ms = 1000,
    };
    galay_coro_task_t cancel_task = {0};
    WaitDriverState cancel_driver = {
        .request = &request,
        .waiter = &cancel_state,
        .generation = generation,
        .action = WaitDriverCancel,
    };
    galay_coro_task_t cancel_driver_task = {0};
    if (expect_code(galay_coro_spawn(&runtime, waiting_entry, &cancel_state, 0, &cancel_task),
                    C_IOResultOk) ||
        expect_code(galay_coro_spawn(&runtime,
                                     wait_driver_entry,
                                     &cancel_driver,
                                     0,
                                     &cancel_driver_task),
                    C_IOResultOk)) {
        return 16;
    }
    if (expect_code(galay_coro_join(&cancel_driver_task, 1000), C_IOResultOk) ||
        expect_code(cancel_driver.destroy_result, C_IOResultInvalid) ||
        expect_code(cancel_driver.finish_result, C_IOResultCancelled) ||
        expect_code(galay_coro_join(&cancel_task, 1000), C_IOResultOk) ||
        cancel_state.result.code != C_IOResultCancelled) {
        return 17;
    }
    if (expect_code(galay_coro_wait_request_complete(&request, generation, ok_result), C_IOResultInvalid)) {
        return 24;
    }
    if (expect_code(galay_coro_destroy(&cancel_driver_task), C_IOResultOk) ||
        expect_code(galay_coro_destroy(&cancel_task), C_IOResultOk)) {
        return 18;
    }

    if (expect_code(galay_coro_wait_request_prepare(&request, &generation), C_IOResultOk) ||
        expect_code(galay_coro_wait_request_event_token_acquire(&request, generation, &token),
                    C_IOResultOk)) {
        return 33;
    }
    void* token_cancel_user_data = 0;
    if (expect_code(galay_coro_wait_event_token_detach_user_data(&token, &token_cancel_user_data),
                    C_IOResultOk)) {
        return 39;
    }
    WaitEntryState token_cancel_state = {
        .request = &request,
        .phase = ATOMIC_VAR_INIT(0),
        .timeout_ms = 1000,
    };
    galay_coro_task_t token_cancel_task = {0};
    WaitDriverState token_cancel_driver = {
        .request = &request,
        .user_data = token_cancel_user_data,
        .waiter = &token_cancel_state,
        .generation = generation,
        .action = WaitDriverCancel,
        .use_user_data = 1,
    };
    galay_coro_task_t token_cancel_driver_task = {0};
    if (expect_code(galay_coro_spawn(&runtime,
                                     waiting_entry,
                                     &token_cancel_state,
                                     0,
                                     &token_cancel_task),
                    C_IOResultOk) ||
        expect_code(galay_coro_spawn(&runtime,
                                     wait_driver_entry,
                                     &token_cancel_driver,
                                     0,
                                     &token_cancel_driver_task),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&token_cancel_driver_task, 1000), C_IOResultOk) ||
        expect_code(token_cancel_driver.destroy_result, C_IOResultInvalid) ||
        expect_code(token_cancel_driver.finish_result, C_IOResultCancelled) ||
        expect_code(galay_coro_join(&token_cancel_task, 1000), C_IOResultOk) ||
        token_cancel_state.result.code != C_IOResultCancelled ||
        expect_code(galay_coro_wait_event_user_data_release(token_cancel_user_data), C_IOResultOk)) {
        return 34;
    }
    if (expect_code(galay_coro_destroy(&token_cancel_driver_task), C_IOResultOk) ||
        expect_code(galay_coro_destroy(&token_cancel_task), C_IOResultOk)) {
        return 35;
    }

    if (expect_code(galay_coro_wait_request_destroy(&request), C_IOResultOk)) {
        return 19;
    }

    (void)galay_kernel_runtime_stop(&runtime);
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess) {
        return 20;
    }
    return 0;
}
