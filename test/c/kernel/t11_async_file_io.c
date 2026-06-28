#include <galay/c/galay-kernel-c/async-c/async_file_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct AsyncFileIoState {
    galay_kernel_async_file_t* file;
    C_IOResult write_result;
    C_IOResult read_result;
    C_IOResult close_result;
    const char* payload;
    size_t payload_size;
    char* read_buffer;
    size_t read_buffer_size;
} AsyncFileIoState;

static int expect_status(C_AsyncFileResultCode actual, C_AsyncFileResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static int make_temp_path(char* path, size_t length)
{
    if (snprintf(path, length, "/tmp/galay_async_file_c_XXXXXX") <= 0) {
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

static void async_file_io_entry(void* arg)
{
    AsyncFileIoState* state = (AsyncFileIoState*)arg;
    state->write_result = galay_kernel_async_file_write(
        state->file, state->payload, state->payload_size, 0, 1000);
    if (state->write_result.code != C_IOResultOk) {
        return;
    }
    state->read_result = galay_kernel_async_file_read(
        state->file, state->read_buffer, state->read_buffer_size, 0, 1000);
    if (state->read_result.code != C_IOResultOk) {
        return;
    }
    state->close_result = galay_kernel_async_file_close(state->file, 1000);
}

int main(void)
{
    C_AsyncFileResultCode codes[] = {
        C_AsyncFileSuccess,
        C_AsyncFileParameterInvalid,
        C_AsyncFileMemoryAllocFailed,
        C_AsyncFileIOFailed,
        C_AsyncFileOperationInvalid,
        C_AsyncFileOperationUnsupported,
    };
    C_AsyncFileOpenMode modes[] = {
        C_AsyncFileOpenModeRead,
        C_AsyncFileOpenModeWrite,
        C_AsyncFileOpenModeReadWrite,
        C_AsyncFileOpenModeAppend,
        C_AsyncFileOpenModeTruncate
    };

    galay_kernel_async_file_t invalid = {0};
    char scratch[8] = {0};
    const char one[] = "x";
    if (codes[0] != C_AsyncFileSuccess ||
        codes[5] != C_AsyncFileOperationUnsupported ||
        modes[2] != C_AsyncFileOpenModeReadWrite ||
        galay_kernel_async_file_get_error(C_AsyncFileSuccess) == 0 ||
        expect_status(galay_kernel_async_file_destroy(0), C_AsyncFileParameterInvalid) ||
        expect_status(galay_kernel_async_file_create(0), C_AsyncFileParameterInvalid) ||
        expect_status(galay_kernel_async_file_open(0, "/tmp/missing", C_AsyncFileOpenModeReadWrite, 0600),
                      C_AsyncFileParameterInvalid) ||
        expect_status(galay_kernel_async_file_size(0, (size_t*)scratch), C_AsyncFileParameterInvalid) ||
        expect_status(galay_kernel_async_file_sync(0), C_AsyncFileParameterInvalid) ||
        expect_code(galay_kernel_async_file_read(0, scratch, sizeof(scratch), 0, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_kernel_async_file_read(&invalid, 0, sizeof(scratch), 0, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_kernel_async_file_write(0, one, sizeof(one) - 1, 0, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_kernel_async_file_write(&invalid, 0, sizeof(one) - 1, 0, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_kernel_async_file_close(0, 0), C_IOResultInvalid)) {
        return 1;
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t file = {0};
    char path[64] = {0};
    char read_buffer[64] = {0};
    size_t file_size = 0;
    const char payload[] = "async-file-c-abi";
    const size_t payload_size = sizeof(payload) - 1;
    int result = 0;

    if (make_temp_path(path, sizeof(path)) != 0) {
        return 2;
    }
    C_AsyncFileResultCode created = galay_kernel_async_file_create(&file);
    if (created == C_AsyncFileOperationUnsupported) {
        if (unlink(path) != 0 && errno != ENOENT) {
            return 11;
        }
        return 0;
    }
    if (created != C_AsyncFileSuccess ||
        galay_kernel_async_file_open(&file, path, C_AsyncFileOpenModeReadWrite, 0600) !=
            C_AsyncFileSuccess ||
        galay_kernel_async_file_size(&file, &file_size) != C_AsyncFileSuccess ||
        file_size != 0) {
        result = 3;
        goto cleanup_file;
    }
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        result = 4;
        goto cleanup_file;
    }

    AsyncFileIoState state = {0};
    state.file = &file;
    state.payload = payload;
    state.payload_size = payload_size;
    state.read_buffer = read_buffer;
    state.read_buffer_size = sizeof(read_buffer);

    galay_coro_task_t task = {0};
    if (expect_code(galay_coro_spawn(&runtime, async_file_io_entry, &state, 0, &task),
                    C_IOResultOk) ||
        expect_code(galay_coro_join(&task, 2000), C_IOResultOk)) {
        result = 5;
    } else if (state.write_result.code != C_IOResultOk ||
               state.write_result.bytes != payload_size ||
               state.read_result.code != C_IOResultOk ||
               state.read_result.bytes != payload_size ||
               memcmp(read_buffer, payload, payload_size) != 0 ||
               state.close_result.code != C_IOResultOk) {
        result = 6;
    }
    if (galay_coro_destroy(&task).code != C_IOResultOk && result == 0) {
        result = 7;
    }

cleanup_file:
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 8;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 9;
        }
    }
    if (file.file != 0) {
        if (galay_kernel_async_file_destroy(&file) != C_AsyncFileSuccess &&
            result == 0) {
            result = 10;
        }
    }
    if (unlink(path) != 0 && errno != ENOENT && result == 0) {
        result = 11;
    }
    return result;
}
