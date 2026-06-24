#include <galay/c/galay-kernel-c/concurrency-c/async_waiter_c.h>

#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

typedef struct WaitState {
    atomic_int done;
    atomic_int code;
} WaitState;

static void on_wait(C_AsyncWaiterResultCode code, void* ctx)
{
    WaitState* state = (WaitState*)ctx;
    atomic_store(&state->code, (int)code);
    atomic_store(&state->done, 1);
}

static int wait_until(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static int wait_until_waiting(galay_kernel_async_waiter_t* waiter)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (galay_kernel_async_waiter_is_waiting(waiter)) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_waiter_t waiter = {0};
    WaitState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_AsyncWaiterIOFailed);

    int exit_code = 0;
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_async_waiter_create(&waiter) != C_AsyncWaiterSuccess ||
        galay_kernel_async_waiter_wait(&runtime, &waiter, on_wait, &state) != C_AsyncWaiterSuccess) {
        exit_code = 1;
        goto cleanup;
    }

    if (wait_until_waiting(&waiter) != 0 ||
        galay_kernel_async_waiter_notify(&waiter) != C_AsyncWaiterSuccess ||
        wait_until(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_AsyncWaiterSuccess ||
        !galay_kernel_async_waiter_is_ready(&waiter)) {
        exit_code = 2;
        goto cleanup;
    }

    printf("async_waiter signal=%s ready=%d\n",
           galay_kernel_async_waiter_get_error((C_AsyncWaiterResultCode)atomic_load(&state.code)),
           galay_kernel_async_waiter_is_ready(&waiter) ? 1 : 0);

cleanup:
    if (waiter.waiter != 0) {
        (void)galay_kernel_async_waiter_destroy(&waiter);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}
