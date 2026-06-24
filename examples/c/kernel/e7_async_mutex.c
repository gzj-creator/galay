#include <galay/c/galay-kernel-c/concurrency-c/async_mutex_c.h>

#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

typedef struct LockState {
    atomic_int done;
    atomic_int code;
    int id;
} LockState;

static void on_lock(C_AsyncMutexResultCode code, void* ctx)
{
    LockState* state = (LockState*)ctx;
    atomic_store(&state->code, (int)code);
    if (code == C_AsyncMutexSuccess) {
        printf("task %d entered critical section\n", state->id);
    }
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
    int exit_code = 0;

    LockState first = {0};
    LockState second = {0};
    atomic_init(&first.done, 0);
    atomic_init(&first.code, (int)C_AsyncMutexIOFailed);
    first.id = 1;
    atomic_init(&second.done, 0);
    atomic_init(&second.code, (int)C_AsyncMutexIOFailed);
    second.id = 2;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_async_mutex_create(&mutex) != C_AsyncMutexSuccess) {
        return 1;
    }

    if (galay_kernel_async_mutex_lock(&runtime, &mutex, on_lock, &first) != C_AsyncMutexSuccess ||
        wait_done(&first.done) != 0 ||
        atomic_load(&first.code) != (int)C_AsyncMutexSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    if (galay_kernel_async_mutex_lock(&runtime, &mutex, on_lock, &second) != C_AsyncMutexSuccess) {
        exit_code = 3;
        goto cleanup;
    }
    if (wait_still_pending(&second.done) != 0) {
        exit_code = 4;
        goto cleanup;
    }

    printf("task 2 is waiting; task 1 leaves critical section\n");
    if (galay_kernel_async_mutex_unlock(&mutex) != C_AsyncMutexSuccess ||
        wait_done(&second.done) != 0 ||
        atomic_load(&second.code) != (int)C_AsyncMutexSuccess) {
        exit_code = 5;
        goto cleanup;
    }

    printf("task 2 leaves critical section\n");
    if (galay_kernel_async_mutex_unlock(&mutex) != C_AsyncMutexSuccess) {
        exit_code = 6;
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
