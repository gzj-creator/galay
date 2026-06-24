#include <galay/c/galay-kernel-c/concurrency-c/async_waiter_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

enum {
    ASYNC_WAITER_ITERATIONS = 1000
};

typedef struct WaitState {
    atomic_int done;
    atomic_int code;
} WaitState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void on_wait(C_AsyncWaiterResultCode code, void* ctx)
{
    WaitState* state = (WaitState*)ctx;
    atomic_store(&state->code, (int)code);
    atomic_store(&state->done, 1);
}

static int wait_done(atomic_int* done)
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

static int wait_waiting(galay_kernel_async_waiter_t* waiter)
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
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        return 1;
    }

    const int64_t start = now_us();
    for (int i = 0; i < ASYNC_WAITER_ITERATIONS; ++i) {
        galay_kernel_async_waiter_t waiter = {0};
        WaitState state;
        atomic_init(&state.done, 0);
        atomic_init(&state.code, (int)C_AsyncWaiterIOFailed);

        if (galay_kernel_async_waiter_create(&waiter) != C_AsyncWaiterSuccess ||
            galay_kernel_async_waiter_wait(&runtime, &waiter, on_wait, &state) != C_AsyncWaiterSuccess ||
            wait_waiting(&waiter) != 0 ||
            galay_kernel_async_waiter_notify(&waiter) != C_AsyncWaiterSuccess ||
            wait_done(&state.done) != 0 ||
            atomic_load(&state.code) != (int)C_AsyncWaiterSuccess ||
            galay_kernel_async_waiter_destroy(&waiter) != C_AsyncWaiterSuccess) {
            if (waiter.waiter != 0) {
                (void)galay_kernel_async_waiter_destroy(&waiter);
            }
            exit_code = 2;
            break;
        }
    }

    const int64_t elapsed = now_us() - start;
    if (exit_code == 0) {
        const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
        const double ops_per_sec = seconds > 0.0 ? (double)ASYNC_WAITER_ITERATIONS / seconds : 0.0;
        printf("async_waiter_signal iterations=%d elapsed_ms=%.3f ops_per_sec=%.2f\n",
               ASYNC_WAITER_ITERATIONS,
               (double)elapsed / 1000.0,
               ops_per_sec);
    }

    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}
