#include <galay/c/galay-kernel-c/concurrency-c/async_mutex_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

enum {
    ASYNC_MUTEX_ITERATIONS = 4096
};

typedef struct BenchmarkState {
    galay_kernel_async_mutex_t* mutex;
    atomic_int completed;
    atomic_int errors;
} BenchmarkState;

typedef struct HolderState {
    atomic_int done;
    atomic_int code;
} HolderState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void on_holder_lock(C_AsyncMutexResultCode code, void* ctx)
{
    HolderState* state = (HolderState*)ctx;
    atomic_store(&state->code, (int)code);
    atomic_store(&state->done, 1);
}

static void on_contended_lock(C_AsyncMutexResultCode code, void* ctx)
{
    BenchmarkState* state = (BenchmarkState*)ctx;
    if (code != C_AsyncMutexSuccess ||
        galay_kernel_async_mutex_unlock(state->mutex) != C_AsyncMutexSuccess) {
        atomic_fetch_add(&state->errors, 1);
    }
    atomic_fetch_add(&state->completed, 1);
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

static int wait_completed(BenchmarkState* state, int expected)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 10000; ++i) {
        if (atomic_load(&state->completed) == expected) {
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
    galay_kernel_async_mutex_t mutex = {0};
    int submitted = 0;
    int exit_code = 0;

    HolderState holder;
    atomic_init(&holder.done, 0);
    atomic_init(&holder.code, (int)C_AsyncMutexIOFailed);

    BenchmarkState state;
    state.mutex = &mutex;
    atomic_init(&state.completed, 0);
    atomic_init(&state.errors, 0);

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_async_mutex_create(&mutex) != C_AsyncMutexSuccess) {
        return 1;
    }

    if (galay_kernel_async_mutex_lock(&runtime, &mutex, on_holder_lock, &holder) != C_AsyncMutexSuccess ||
        wait_done(&holder.done) != 0 ||
        atomic_load(&holder.code) != (int)C_AsyncMutexSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    for (int i = 0; i < ASYNC_MUTEX_ITERATIONS; ++i) {
        if (galay_kernel_async_mutex_lock(&runtime, &mutex, on_contended_lock, &state) != C_AsyncMutexSuccess) {
            exit_code = 3;
            goto cleanup;
        }
        ++submitted;
    }

    {
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, 0);
    }

    const int64_t start = now_us();
    if (galay_kernel_async_mutex_unlock(&mutex) != C_AsyncMutexSuccess ||
        wait_completed(&state, submitted) != 0 ||
        atomic_load(&state.errors) != 0) {
        exit_code = 4;
        goto cleanup;
    }
    const int64_t elapsed = now_us() - start;
    const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
    const double ops_per_sec = seconds > 0.0 ? (double)submitted / seconds : 0.0;
    printf("async_mutex_contended iterations=%d elapsed_ms=%.3f ops_per_sec=%.2f\n",
           submitted,
           (double)elapsed / 1000.0,
           ops_per_sec);

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
