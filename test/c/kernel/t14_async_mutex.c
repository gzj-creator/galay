#include <galay/c/galay-kernel-c/concurrency-c/async_mutex_c.h>

#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>

typedef struct CallbackState {
    atomic_int done;
    atomic_int code;
} CallbackState;

static int expect_status(C_AsyncMutexResultCode actual, C_AsyncMutexResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static void reset_callback_state(CallbackState* state, C_AsyncMutexResultCode initial_code)
{
    atomic_store(&state->done, 0);
    atomic_store(&state->code, (int)initial_code);
}

static void on_lock(C_AsyncMutexResultCode code, void* ctx)
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

static int wait_still_pending(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 50; ++i) {
        if (atomic_load(done)) {
            return 1;
        }
        nanosleep(&pause, 0);
    }
    return 0;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_mutex_t mutex = {0};
    CallbackState first;
    CallbackState second;
    CallbackState timeout_state;
    CallbackState stopped_state;
    int exit_code = 0;

    atomic_init(&first.done, 0);
    atomic_init(&first.code, (int)C_AsyncMutexIOFailed);
    atomic_init(&second.done, 0);
    atomic_init(&second.code, (int)C_AsyncMutexIOFailed);
    atomic_init(&timeout_state.done, 0);
    atomic_init(&timeout_state.code, (int)C_AsyncMutexIOFailed);
    atomic_init(&stopped_state.done, 0);
    atomic_init(&stopped_state.code, (int)C_AsyncMutexIOFailed);

    if (galay_kernel_async_mutex_get_error(C_AsyncMutexSuccess) == 0) {
        return 1;
    }
    if (expect_status(galay_kernel_async_mutex_create(0), C_AsyncMutexParameterInvalid)) {
        return 2;
    }
    if (expect_status(galay_kernel_async_mutex_destroy(0), C_AsyncMutexParameterInvalid)) {
        return 3;
    }
    if (galay_kernel_async_mutex_is_locked(0)) {
        return 4;
    }
    if (expect_status(galay_kernel_async_mutex_destroy(&mutex), C_AsyncMutexSuccess)) {
        return 5;
    }
    if (expect_status(galay_kernel_async_mutex_create(&mutex), C_AsyncMutexSuccess)) {
        return 6;
    }
    if (mutex.mutex == 0 || galay_kernel_async_mutex_is_locked(&mutex)) {
        return 7;
    }
    if (expect_status(galay_kernel_async_mutex_lock(0, &mutex, on_lock, &first), C_AsyncMutexParameterInvalid)) {
        return 8;
    }
    if (expect_status(galay_kernel_async_mutex_lock(&runtime, &mutex, 0, &first), C_AsyncMutexParameterInvalid)) {
        return 9;
    }
    if (expect_status(galay_kernel_async_mutex_lock_timeout(&runtime, &mutex, 1, 0, &first), C_AsyncMutexParameterInvalid)) {
        return 10;
    }

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        exit_code = 11;
        goto cleanup;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 12;
        goto cleanup;
    }

    reset_callback_state(&first, C_AsyncMutexIOFailed);
    if (expect_status(galay_kernel_async_mutex_lock(&runtime, &mutex, on_lock, &first), C_AsyncMutexSuccess) ||
        wait_done(&first.done) != 0 ||
        atomic_load(&first.code) != (int)C_AsyncMutexSuccess ||
        !galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 13;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_mutex_unlock(&mutex), C_AsyncMutexSuccess) ||
        galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 14;
        goto cleanup;
    }

    reset_callback_state(&first, C_AsyncMutexIOFailed);
    reset_callback_state(&second, C_AsyncMutexIOFailed);
    if (expect_status(galay_kernel_async_mutex_lock(&runtime, &mutex, on_lock, &first), C_AsyncMutexSuccess) ||
        wait_done(&first.done) != 0 ||
        atomic_load(&first.code) != (int)C_AsyncMutexSuccess ||
        !galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 15;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_mutex_lock(&runtime, &mutex, on_lock, &second), C_AsyncMutexSuccess)) {
        exit_code = 16;
        goto cleanup;
    }
    if (wait_still_pending(&second.done) != 0) {
        exit_code = 17;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_mutex_unlock(&mutex), C_AsyncMutexSuccess) ||
        wait_done(&second.done) != 0 ||
        atomic_load(&second.code) != (int)C_AsyncMutexSuccess ||
        !galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 18;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_mutex_unlock(&mutex), C_AsyncMutexSuccess) ||
        galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 19;
        goto cleanup;
    }

    reset_callback_state(&first, C_AsyncMutexIOFailed);
    reset_callback_state(&timeout_state, C_AsyncMutexIOFailed);
    if (expect_status(galay_kernel_async_mutex_lock(&runtime, &mutex, on_lock, &first), C_AsyncMutexSuccess) ||
        wait_done(&first.done) != 0 ||
        atomic_load(&first.code) != (int)C_AsyncMutexSuccess ||
        !galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 20;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_mutex_lock_timeout(&runtime, &mutex, 20, on_lock, &timeout_state), C_AsyncMutexSuccess) ||
        wait_done(&timeout_state.done) != 0 ||
        atomic_load(&timeout_state.code) != (int)C_AsyncMutexTimeout ||
        !galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 21;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_mutex_unlock(&mutex), C_AsyncMutexSuccess) ||
        galay_kernel_async_mutex_is_locked(&mutex)) {
        exit_code = 22;
        goto cleanup;
    }

    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_is_running(&runtime)) {
        exit_code = 23;
        goto cleanup;
    }
    reset_callback_state(&stopped_state, C_AsyncMutexIOFailed);
    if (expect_status(galay_kernel_async_mutex_lock(&runtime, &mutex, on_lock, &stopped_state), C_AsyncMutexRuntimeNotRunning) ||
        atomic_load(&stopped_state.done) != 0) {
        exit_code = 24;
        goto cleanup;
    }

cleanup:
    if (mutex.mutex != 0) {
        (void)galay_kernel_async_mutex_destroy(&mutex);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}
