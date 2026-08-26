#include <galay/c/galay-kernel-c/async-c/async_file.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct AsyncFileTimeoutState {
    galay_c_async_file_t* file;
    C_IOResult zero_read_result;
    C_IOResult zero_write_result;
    C_IOResult close_result;
    char buffer[8];
} AsyncFileTimeoutState;

static int expect_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static int make_temp_path(char* path, size_t length)
{
    if (snprintf(path, length, "/tmp/galay_async_file_timeout_XXXXXX") <= 0) {
        return 1;
    }
    int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    if (close(fd) != 0) {
        return 1;
    }
    if (unlink(path) != 0) {
        return 1;
    }
    return 0;
}

static void timeout_entry(void* arg)
{
    AsyncFileTimeoutState* state = (AsyncFileTimeoutState*)arg;
    state->zero_read_result =
        galay_c_async_file_read(state->file, state->buffer, sizeof(state->buffer), 0);
    state->zero_write_result =
        galay_c_async_file_write(state->file, "x", 1, 0);
    state->close_result = galay_c_async_file_close(state->file);
}

int main(void)
{
    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.parallel_scheduler_count = 0;

    galay_c_runtime_t runtime = {0};
    galay_c_async_file_t file = {.fd = -1};
    char path[64] = {0};
    int result = 0;

    if (make_temp_path(path, sizeof(path)) != 0) {
        return 1;
    }
    if (galay_c_async_file_open(
            &file, path,
            GALAY_C_FILE_RDWR | GALAY_C_FILE_CREATE | GALAY_C_FILE_TRUNC,
            0600).code != C_IOResultOk ||
        galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_c_runtime_start(&runtime) != C_RuntimeSuccess) {
        result = 2;
        goto cleanup;
    }

    AsyncFileTimeoutState state = {0};
    state.file = &file;
    galay_c_coro_task_t task = {0};
    if (expect_code(galay_c_coro_spawn(&runtime, timeout_entry, &state, 0, &task),
                    C_IOResultOk) ||
        expect_code(galay_c_coro_join(&task, 2000), C_IOResultOk)) {
        result = 3;
    } else if (state.zero_read_result.code != C_IOResultEof ||
               state.zero_write_result.code != C_IOResultOk ||
               state.zero_write_result.bytes != 1 ||
               state.close_result.code != C_IOResultOk) {
        result = 4;
    }
    if (galay_c_coro_destroy(&task).code != C_IOResultOk && result == 0) {
        result = 5;
    }

cleanup:
    if (file.fd >= 0) {
        if (galay_c_async_file_close(&file).code != C_IOResultOk && result == 0) {
            result = 6;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 7;
        }
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 8;
        }
    }
    if (unlink(path) != 0 && errno != ENOENT && result == 0) {
        result = 9;
    }
    return result;
}
