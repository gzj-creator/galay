#include <galay/c/galay-kernel-c/async-c/async_file.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

enum { ITERATIONS = 256 };

typedef struct FilePollState {
    galay_c_async_file_t* file;
    int result;
    int64_t elapsed_us;
} FilePollState;

static int64_t now_us(void)
{
    struct timeval value;
    return gettimeofday(&value, NULL) == 0
        ? (int64_t)value.tv_sec * 1000000 + value.tv_usec
        : -1;
}

static void poll_entry(void* arg)
{
    FilePollState* const state = arg;
    char byte = 0;
    const int64_t start = now_us();
    for (int iteration = 0; iteration < ITERATIONS; ++iteration) {
        const C_IOResult end = galay_c_async_file_seek(state->file, 0, SEEK_END);
        const C_IOResult read = galay_c_async_file_read(state->file, &byte, 1, 0);
        if (end.code != C_IOResultOk || read.code != C_IOResultEof) {
            state->result = 1;
            return;
        }
    }
    state->elapsed_us = now_us() - start;
}

int main(void)
{
    char path[] = "/tmp/galay-async-file-poll-XXXXXX";
    const int temp_fd = mkstemp(path);
    if (temp_fd < 0 || close(temp_fd) != 0) {
        return 1;
    }

    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_async_file_t file = {.fd = -1};
    galay_c_coro_task_t task = {0};
    FilePollState state = {.file = &file};
    int result = 0;

    if (galay_c_async_file_open(
            &file, path,
            GALAY_C_FILE_RDWR | GALAY_C_FILE_CREATE | GALAY_C_FILE_TRUNC,
            0600).code != C_IOResultOk ||
        galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_c_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_c_coro_spawn(&runtime, poll_entry, &state, NULL, &task).code != C_IOResultOk ||
        galay_c_coro_join(&task, 5000).code != C_IOResultOk || state.result != 0) {
        result = 2;
        goto cleanup;
    }
    if (printf("async_file_poll iterations=%d elapsed_ms=%.3f polls_per_sec=%.2f\n",
               ITERATIONS,
               (double)state.elapsed_us / 1000.0,
               state.elapsed_us > 0 ? (double)ITERATIONS * 1000000.0 / state.elapsed_us
                                    : 0.0) < 0) {
        result = 3;
    }

cleanup:
    if (task.task != NULL && galay_c_coro_destroy(&task).code != C_IOResultOk && result == 0) {
        result = 4;
    }
    if (file.fd >= 0 && galay_c_async_file_close(&file).code != C_IOResultOk && result == 0) {
        result = 5;
    }
    if (runtime.runtime != NULL) {
        if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 6;
        }
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 7;
        }
    }
    if (unlink(path) != 0 && errno != ENOENT && result == 0) {
        result = 8;
    }
    return result;
}
