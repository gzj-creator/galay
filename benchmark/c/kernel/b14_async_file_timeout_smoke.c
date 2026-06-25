#include <galay/c/galay-kernel-c/async-c/async_file_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct TimeoutState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
} TimeoutState;

static void on_read(galay_kernel_async_file_read_result_t* result, void* ctx)
{
    TimeoutState* state = (TimeoutState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_AsyncFileIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    atomic_store(&state->done, 1);
}

static void on_write(galay_kernel_async_file_write_result_t* result, void* ctx)
{
    TimeoutState* state = (TimeoutState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_AsyncFileIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    atomic_store(&state->done, 1);
}

static void on_close(C_AsyncFileResultCode code, void* ctx)
{
    TimeoutState* state = (TimeoutState*)ctx;
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

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int make_temp_path(char* path, size_t length)
{
    if (snprintf(path, length, "/tmp/galay_async_file_timeout_smoke_XXXXXX") <= 0) {
        return 1;
    }

    int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    close(fd);
    unlink(path);
    return 0;
}

static int close_async_file(galay_kernel_runtime_t* runtime, galay_kernel_async_file_t* file)
{
    TimeoutState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_AsyncFileIOFailed);
    atomic_init(&state.bytes, -1);

    if (file->file == 0) {
        return 0;
    }
    if (galay_kernel_async_file_close(runtime, file, on_close, &state) != C_AsyncFileSuccess) {
        return 1;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_AsyncFileSuccess) {
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    int iterations = 32;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0) {
            iterations = parsed;
        }
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t file = {0};
    char path[64] = {0};
    char read_buffer[8] = {0};
    const char payload[] = "galay";
    const size_t payload_size = sizeof(payload) - 1;
    int completed = 0;
    int failures = 0;

    if (make_temp_path(path, sizeof(path)) != 0 ||
        galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        failures = 1;
        goto cleanup;
    }

    C_AsyncFileResultCode created = galay_kernel_async_file_create(&file);
    if (created == C_AsyncFileOperationUnsupported) {
        printf("async_file_timeout_smoke unsupported: %s\n", galay_kernel_async_file_get_error(created));
        completed = iterations;
        goto cleanup;
    }
    if (created != C_AsyncFileSuccess ||
        galay_kernel_async_file_open(&file, path, C_AsyncFileOpenModeReadWrite, 0600) != C_AsyncFileSuccess) {
        failures = 1;
        goto cleanup;
    }

    TimeoutState write_state;
    atomic_init(&write_state.done, 0);
    atomic_init(&write_state.code, (int)C_AsyncFileIOFailed);
    atomic_init(&write_state.bytes, -1);
    if (galay_kernel_async_file_write_timeout(&runtime, &file, payload, payload_size, 0, 1000, on_write, &write_state) !=
            C_AsyncFileSuccess ||
        wait_done(&write_state.done) != 0 ||
        atomic_load(&write_state.code) != (int)C_AsyncFileSuccess ||
        atomic_load(&write_state.bytes) != (int)payload_size ||
        galay_kernel_async_file_sync(&file) != C_AsyncFileSuccess) {
        failures = 1;
        goto cleanup;
    }

    const uint64_t start_ns = now_ns();
    for (int i = 0; i < iterations; ++i) {
        TimeoutState state;
        atomic_init(&state.done, 0);
        atomic_init(&state.code, (int)C_AsyncFileIOFailed);
        atomic_init(&state.bytes, -1);
        memset(read_buffer, 0, sizeof(read_buffer));

        C_AsyncFileResultCode submitted = galay_kernel_async_file_read_timeout(
            &runtime,
            &file,
            read_buffer,
            sizeof(read_buffer),
            0,
            1000,
            on_read,
            &state);
        if (submitted != C_AsyncFileSuccess) {
            printf("async_file_timeout_smoke submit failed: %s\n",
                   galay_kernel_async_file_get_error(submitted));
            ++failures;
            break;
        }
        const int waited = wait_done(&state.done);
        const int code = atomic_load(&state.code);
        const int bytes = atomic_load(&state.bytes);
        if (waited != 0 ||
            code != (int)C_AsyncFileSuccess ||
            bytes != (int)payload_size ||
            memcmp(read_buffer, payload, payload_size) != 0) {
            printf("async_file_timeout_smoke callback failed: done=%d code=%s bytes=%d\n",
                   atomic_load(&state.done),
                   galay_kernel_async_file_get_error((C_AsyncFileResultCode)code),
                   bytes);
            ++failures;
            break;
        }
        ++completed;
    }
    const uint64_t elapsed_ns = now_ns() - start_ns;

    printf("async_file_timeout_smoke iterations=%d completed=%d failures=%d elapsed_ns=%llu avg_ns=%.2f\n",
           iterations,
           completed,
           failures,
           (unsigned long long)elapsed_ns,
           completed == 0 ? 0.0 : (double)elapsed_ns / (double)completed);

cleanup:
    if (file.file != 0) {
        (void)close_async_file(&runtime, &file);
        (void)galay_kernel_async_file_destroy(&file);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    unlink(path);
    return failures == 0 && completed == iterations ? 0 : 1;
}
