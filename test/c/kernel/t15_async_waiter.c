#include <galay/c/galay-kernel-c/concurrency-c/async_waiter_c.h>

#include <stdatomic.h>
#include <time.h>

typedef struct CallbackState {
    atomic_int done;
    atomic_int code;
} CallbackState;

static int expect_status(C_AsyncWaiterResultCode actual, C_AsyncWaiterResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static void on_wait(C_AsyncWaiterResultCode code, void* ctx)
{
    CallbackState* state = (CallbackState*)ctx;
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

static void init_callback_state(CallbackState* state)
{
    atomic_init(&state->done, 0);
    atomic_init(&state->code, (int)C_AsyncWaiterIOFailed);
}

static int create_started_runtime(galay_kernel_runtime_t* runtime)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    if (galay_kernel_runtime_create(&config, runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(runtime) != C_RuntimeSuccess) {
        (void)galay_kernel_runtime_destroy(runtime);
        return 1;
    }
    return 0;
}

static int test_notify_before_wait(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_waiter_t waiter = {0};
    CallbackState state;
    int exit_code = 0;
    init_callback_state(&state);

    if (create_started_runtime(&runtime) != 0) {
        return 1;
    }
    if (expect_status(galay_kernel_async_waiter_create(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 2;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_notify(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 3;
        goto cleanup;
    }
    if (!galay_kernel_async_waiter_is_ready(&waiter) ||
        galay_kernel_async_waiter_is_waiting(&waiter)) {
        exit_code = 4;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_wait(&runtime, &waiter, on_wait, &state), C_AsyncWaiterSuccess)) {
        exit_code = 5;
        goto cleanup;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_AsyncWaiterSuccess) {
        exit_code = 6;
        goto cleanup;
    }

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

static int test_wait_before_notify(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_waiter_t waiter = {0};
    CallbackState state;
    int exit_code = 0;
    init_callback_state(&state);

    if (create_started_runtime(&runtime) != 0) {
        return 10;
    }
    if (expect_status(galay_kernel_async_waiter_create(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 11;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_wait(&runtime, &waiter, on_wait, &state), C_AsyncWaiterSuccess)) {
        exit_code = 12;
        goto cleanup;
    }
    for (int i = 0; i < 200; ++i) {
        if (galay_kernel_async_waiter_is_waiting(&waiter)) {
            break;
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, 0);
    }
    if (!galay_kernel_async_waiter_is_waiting(&waiter) ||
        galay_kernel_async_waiter_is_ready(&waiter)) {
        exit_code = 13;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_notify(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 14;
        goto cleanup;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_AsyncWaiterSuccess ||
        !galay_kernel_async_waiter_is_ready(&waiter)) {
        exit_code = 15;
        goto cleanup;
    }

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

static int test_duplicate_notify(void)
{
    galay_kernel_async_waiter_t waiter = {0};
    int exit_code = 0;

    if (expect_status(galay_kernel_async_waiter_create(&waiter), C_AsyncWaiterSuccess)) {
        return 20;
    }
    if (expect_status(galay_kernel_async_waiter_notify(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 21;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_notify(&waiter), C_AsyncWaiterOperationInvalid)) {
        exit_code = 22;
        goto cleanup;
    }

cleanup:
    if (waiter.waiter != 0) {
        (void)galay_kernel_async_waiter_destroy(&waiter);
    }
    return exit_code;
}

static int test_timeout(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_waiter_t waiter = {0};
    CallbackState state;
    int exit_code = 0;
    init_callback_state(&state);

    if (create_started_runtime(&runtime) != 0) {
        return 30;
    }
    if (expect_status(galay_kernel_async_waiter_create(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 31;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_wait_timeout(&runtime, &waiter, 20, on_wait, &state), C_AsyncWaiterSuccess)) {
        exit_code = 32;
        goto cleanup;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_AsyncWaiterTimeout ||
        galay_kernel_async_waiter_is_ready(&waiter)) {
        exit_code = 33;
        goto cleanup;
    }

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

static int test_stopped_runtime(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_waiter_t waiter = {0};
    CallbackState state;
    int exit_code = 0;
    init_callback_state(&state);

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 40;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 41;
        goto cleanup;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess) {
        exit_code = 42;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_create(&waiter), C_AsyncWaiterSuccess)) {
        exit_code = 43;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_wait(&runtime, &waiter, on_wait, &state), C_AsyncWaiterRuntimeNotRunning)) {
        exit_code = 44;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_waiter_wait_timeout(&runtime, &waiter, 10, on_wait, &state), C_AsyncWaiterRuntimeNotRunning)) {
        exit_code = 45;
        goto cleanup;
    }
    if (atomic_load(&state.done) != 0) {
        exit_code = 46;
        goto cleanup;
    }

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

int main(void)
{
    int result = test_notify_before_wait();
    if (result != 0) {
        return result;
    }
    result = test_wait_before_notify();
    if (result != 0) {
        return result;
    }
    result = test_duplicate_notify();
    if (result != 0) {
        return result;
    }
    result = test_timeout();
    if (result != 0) {
        return result;
    }
    return test_stopped_runtime();
}
