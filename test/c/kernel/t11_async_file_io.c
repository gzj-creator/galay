#include <galay/c/galay-kernel-c/async-c/async_file.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct AsyncFileIoState {
    galay_c_async_file_t* file;
    C_IOResult write_result;
    C_IOResult seek_result;
    C_IOResult read_result;
    C_IOResult close_result;
    const char* payload;
    size_t payload_size;
    char* read_buffer;
    size_t read_buffer_size;
} AsyncFileIoState;

static int expect_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static int make_temp_path(char* path, size_t length)
{
    if (snprintf(path, length, "/tmp/galay_async_file_XXXXXX") <= 0) {
        return 1;
    }
    const int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    if (close(fd) != 0) {
        return 1;
    }
    return unlink(path) == 0 ? 0 : 1;
}

static void async_file_io_entry(void* arg)
{
    AsyncFileIoState* const state = arg;
    state->write_result = galay_c_async_file_write(
        state->file, state->payload, state->payload_size, 1000);
    if (state->write_result.code != C_IOResultOk) {
        return;
    }
    state->seek_result = galay_c_async_file_seek(state->file, 0, SEEK_SET);
    if (state->seek_result.code != C_IOResultOk) {
        return;
    }
    state->read_result = galay_c_async_file_read(
        state->file, state->read_buffer, state->read_buffer_size, 1000);
    if (state->read_result.code != C_IOResultOk) {
        return;
    }
    state->close_result = galay_c_async_file_close(state->file);
}

int main(void)
{
    galay_c_async_file_t invalid = {.fd = -1};
    char scratch[8] = {0};
    const char one[] = "x";
    if (expect_code(galay_c_async_file_open(NULL, "/tmp/missing", GALAY_C_FILE_READ, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_c_async_file_open(&invalid, NULL, GALAY_C_FILE_READ, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_c_async_file_read(NULL, scratch, sizeof(scratch), 0),
                    C_IOResultInvalid) ||
        expect_code(galay_c_async_file_read(&invalid, NULL, sizeof(scratch), 0),
                    C_IOResultInvalid) ||
        expect_code(galay_c_async_file_write(NULL, one, sizeof(one) - 1, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_c_async_file_write(&invalid, NULL, sizeof(one) - 1, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_c_async_file_close(NULL), C_IOResultInvalid) ||
        expect_code(galay_c_async_file_close(&invalid), C_IOResultOk)) {
        return 1;
    }

    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_c_runtime_t runtime = {0};
    galay_c_async_file_t file = {.fd = -1};
    galay_c_coro_task_t task = {0};
    char path[64] = {0};
    char read_buffer[64] = {0};
    const char payload[] = "async-file-c-abi";
    const size_t payload_size = sizeof(payload) - 1;
    int result = 0;

    if (make_temp_path(path, sizeof(path)) != 0) {
        return 2;
    }
    if (expect_code(galay_c_async_file_open(
                        &file, path,
                        GALAY_C_FILE_RDWR | GALAY_C_FILE_CREATE | GALAY_C_FILE_TRUNC,
                        0600),
                    C_IOResultOk) ||
        galay_c_async_file_tell(&file).value != 0) {
        result = 3;
        goto cleanup;
    }
    if (galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_c_runtime_start(&runtime) != C_RuntimeSuccess) {
        result = 4;
        goto cleanup;
    }

    AsyncFileIoState state = {
        .file = &file,
        .payload = payload,
        .payload_size = payload_size,
        .read_buffer = read_buffer,
        .read_buffer_size = sizeof(read_buffer),
    };
    if (expect_code(galay_c_coro_spawn(&runtime, async_file_io_entry, &state, NULL, &task),
                    C_IOResultOk) ||
        expect_code(galay_c_coro_join(&task, 2000), C_IOResultOk)) {
        result = 5;
    } else if (state.write_result.code != C_IOResultOk ||
               state.write_result.bytes != payload_size ||
               state.seek_result.code != C_IOResultOk || state.seek_result.value != 0 ||
               state.read_result.code != C_IOResultOk ||
               state.read_result.bytes != payload_size ||
               memcmp(read_buffer, payload, payload_size) != 0 ||
               state.close_result.code != C_IOResultOk) {
        result = 6;
    }

cleanup:
    if (task.task != NULL && galay_c_coro_destroy(&task).code != C_IOResultOk && result == 0) {
        result = 7;
    }
    if (file.fd >= 0 && galay_c_async_file_close(&file).code != C_IOResultOk && result == 0) {
        result = 8;
    }
    if (runtime.runtime != NULL) {
        if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 9;
        }
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 10;
        }
    }
    if (unlink(path) != 0 && errno != ENOENT && result == 0) {
        result = 11;
    }
    return result;
}
