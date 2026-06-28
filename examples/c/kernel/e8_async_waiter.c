#include <galay/c/galay-kernel-c/concurrency-c/async_waiter_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>

typedef struct WaitState {
    galay_kernel_async_waiter_t* waiter;
    C_IOResult result;
} WaitState;

static void wait_entry(void* ctx)
{
    WaitState* state = (WaitState*)ctx;
    state->result = galay_kernel_async_waiter_wait(state->waiter, 2000);
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_waiter_t waiter = {0};
    galay_coro_task_t task = {0};
    WaitState state = {&waiter, {0}};

    int exit_code = 0;
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_async_waiter_create(&waiter) != C_AsyncWaiterSuccess ||
        galay_coro_spawn(&runtime, wait_entry, &state, 0, &task).code != C_IOResultOk ||
        galay_kernel_async_waiter_notify(&waiter) != C_AsyncWaiterSuccess ||
        galay_coro_join(&task, 3000).code != C_IOResultOk ||
        state.result.code != C_IOResultOk ||
        !galay_kernel_async_waiter_is_ready(&waiter)) {
        exit_code = 1;
        goto cleanup;
    }

    if (printf("async_waiter signal=%s ready=%d\n",
               galay_kernel_async_waiter_get_error(C_AsyncWaiterSuccess),
               galay_kernel_async_waiter_is_ready(&waiter) ? 1 : 0) < 0) {
        exit_code = 2;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 3;
        }
    }
    if (waiter.waiter != 0) {
        if (galay_kernel_async_waiter_destroy(&waiter) != C_AsyncWaiterSuccess && exit_code == 0) {
            exit_code = 4;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 5;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 6;
        }
    }
    return exit_code;
}
