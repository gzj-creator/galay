#include <galay/c/galay-kernel-c/async-c/async_file_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct IoState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
    const void* buffer;
    size_t length;
    size_t offset;
} IoState;

typedef struct CloseState {
    atomic_int done;
    atomic_int code;
} CloseState;

static int expect_status(C_AsyncFileResultCode actual, C_AsyncFileResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static void reset_io_state(IoState* state)
{
    atomic_store(&state->done, 0);
    atomic_store(&state->code, (int)C_AsyncFileIOFailed);
    atomic_store(&state->bytes, -1);
    state->buffer = 0;
    state->length = 0;
    state->offset = 0;
}

static void reset_close_state(CloseState* state)
{
    atomic_store(&state->done, 0);
    atomic_store(&state->code, (int)C_AsyncFileIOFailed);
}

static void on_read(galay_kernel_async_file_read_result_t* result, void* ctx)
{
    IoState* state = (IoState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_AsyncFileIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    state->buffer = result == 0 ? 0 : result->buffer;
    state->length = result == 0 ? 0 : result->length;
    state->offset = result == 0 ? 0 : result->offset;
    atomic_store(&state->done, 1);
}

static void on_write(galay_kernel_async_file_write_result_t* result, void* ctx)
{
    IoState* state = (IoState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_AsyncFileIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    state->buffer = result == 0 ? 0 : result->buffer;
    state->length = result == 0 ? 0 : result->length;
    state->offset = result == 0 ? 0 : result->offset;
    atomic_store(&state->done, 1);
}

static void on_close(C_AsyncFileResultCode code, void* ctx)
{
    CloseState* state = (CloseState*)ctx;
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

static int make_temp_path(char* path, size_t length)
{
    if (snprintf(path, length, "/tmp/galay_async_file_timeout_XXXXXX") <= 0) {
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

static int test_parameter_errors(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t file = {0};
    char read_buffer[8] = {0};
    const char write_buffer[] = "x";
    IoState io_state;
    CloseState close_state;

    atomic_init(&io_state.done, 0);
    atomic_init(&io_state.code, (int)C_AsyncFileIOFailed);
    atomic_init(&io_state.bytes, -1);
    atomic_init(&close_state.done, 0);
    atomic_init(&close_state.code, (int)C_AsyncFileIOFailed);

    if (expect_status(galay_kernel_async_file_read_timeout(0, &file, read_buffer,
                                                           sizeof(read_buffer), 0, 1, on_read, &io_state),
                      C_AsyncFileParameterInvalid)) {
        return 1;
    }
    if (expect_status(galay_kernel_async_file_read_timeout(&runtime, 0, read_buffer,
                                                           sizeof(read_buffer), 0, 1, on_read, &io_state),
                      C_AsyncFileParameterInvalid)) {
        return 2;
    }
    if (expect_status(galay_kernel_async_file_read_timeout(&runtime, &file, 0,
                                                           sizeof(read_buffer), 0, 1, on_read, &io_state),
                      C_AsyncFileParameterInvalid)) {
        return 3;
    }
    if (expect_status(galay_kernel_async_file_read_timeout(&runtime, &file, read_buffer,
                                                           0, 0, 1, on_read, &io_state),
                      C_AsyncFileParameterInvalid)) {
        return 4;
    }
    if (expect_status(galay_kernel_async_file_read_timeout(&runtime, &file, read_buffer,
                                                           sizeof(read_buffer), 0, 1, 0, &io_state),
                      C_AsyncFileParameterInvalid)) {
        return 5;
    }

    if (expect_status(galay_kernel_async_file_write_timeout(0, &file, write_buffer,
                                                            sizeof(write_buffer) - 1, 0, 1, on_write, &io_state),
                      C_AsyncFileParameterInvalid)) {
        return 6;
    }
    if (expect_status(galay_kernel_async_file_write_timeout(&runtime, 0, write_buffer,
                                                            sizeof(write_buffer) - 1, 0, 1, on_write, &io_state),
                      C_AsyncFileParameterInvalid)) {
        return 7;
    }
    if (expect_status(galay_kernel_async_file_write_timeout(&runtime, &file, 0,
                                                            sizeof(write_buffer) - 1, 0, 1, on_write, &io_state),
                      C_AsyncFileParameterInvalid)) {
        return 8;
    }
    if (expect_status(galay_kernel_async_file_write_timeout(&runtime, &file, write_buffer,
                                                            0, 0, 1, on_write, &io_state),
                      C_AsyncFileParameterInvalid)) {
        return 9;
    }
    if (expect_status(galay_kernel_async_file_write_timeout(&runtime, &file, write_buffer,
                                                            sizeof(write_buffer) - 1, 0, 1, 0, &io_state),
                      C_AsyncFileParameterInvalid)) {
        return 10;
    }

    if (expect_status(galay_kernel_async_file_close_timeout(0, &file, 1, on_close, &close_state),
                      C_AsyncFileParameterInvalid)) {
        return 11;
    }
    if (expect_status(galay_kernel_async_file_close_timeout(&runtime, 0, 1, on_close, &close_state),
                      C_AsyncFileParameterInvalid)) {
        return 12;
    }
    if (expect_status(galay_kernel_async_file_close_timeout(&runtime, &file, 1, 0, &close_state),
                      C_AsyncFileParameterInvalid)) {
        return 13;
    }

    if (atomic_load(&io_state.done) || atomic_load(&close_state.done)) {
        return 14;
    }

    return 0;
}

#if defined(USE_KQUEUE) || defined(USE_IOURING)
static int test_supported_timeout_callback(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    const char payload[] = "timeout";
    const size_t payload_size = sizeof(payload) - 1;
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t file = {0};
    char path[64] = {0};
    char read_buffer[8] = {0};
    IoState read_state;
    IoState write_state;
    CloseState close_state;
    int exit_code = 0;

    atomic_init(&read_state.done, 0);
    atomic_init(&read_state.code, (int)C_AsyncFileIOFailed);
    atomic_init(&read_state.bytes, -1);
    atomic_init(&write_state.done, 0);
    atomic_init(&write_state.code, (int)C_AsyncFileIOFailed);
    atomic_init(&write_state.bytes, -1);
    atomic_init(&close_state.done, 0);
    atomic_init(&close_state.code, (int)C_AsyncFileIOFailed);

    if (make_temp_path(path, sizeof(path)) != 0) {
        return 1;
    }
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_async_file_create(&file) != C_AsyncFileSuccess ||
        galay_kernel_async_file_open(&file, path, C_AsyncFileOpenModeReadWrite, 0600) != C_AsyncFileSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    reset_io_state(&write_state);
    if (galay_kernel_async_file_write_timeout(&runtime, &file, payload,
                                              payload_size, 0, 1000, on_write, &write_state) != C_AsyncFileSuccess ||
        wait_done(&write_state.done) != 0 ||
        atomic_load(&write_state.code) != (int)C_AsyncFileSuccess ||
        atomic_load(&write_state.bytes) != (int)payload_size ||
        write_state.buffer != payload ||
        write_state.length != payload_size ||
        write_state.offset != 0) {
        exit_code = 3;
        goto cleanup;
    }

    if (galay_kernel_async_file_sync(&file) != C_AsyncFileSuccess) {
        exit_code = 4;
        goto cleanup;
    }

    reset_io_state(&read_state);
    if (galay_kernel_async_file_read_timeout(&runtime, &file, read_buffer,
                                             sizeof(read_buffer), 0, 1000, on_read, &read_state) != C_AsyncFileSuccess ||
        wait_done(&read_state.done) != 0 ||
        atomic_load(&read_state.code) != (int)C_AsyncFileSuccess ||
        atomic_load(&read_state.bytes) != (int)payload_size ||
        read_state.buffer != read_buffer ||
        read_state.length != sizeof(read_buffer) ||
        read_state.offset != 0 ||
        memcmp(read_buffer, payload, payload_size) != 0) {
        exit_code = 5;
        goto cleanup;
    }

    reset_close_state(&close_state);
    if (galay_kernel_async_file_close_timeout(&runtime, &file, 1000, on_close, &close_state) != C_AsyncFileSuccess ||
        wait_done(&close_state.done) != 0 ||
        atomic_load(&close_state.code) != (int)C_AsyncFileSuccess) {
        exit_code = 6;
        goto cleanup;
    }

cleanup:
    if (file.file != 0) {
        (void)galay_kernel_async_file_destroy(&file);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    unlink(path);
    return exit_code;
}
#else
static int test_unsupported_backend(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t file = {0};
    char read_buffer[8] = {0};
    const char write_buffer[] = "x";
    IoState io_state;
    CloseState close_state;

    atomic_init(&io_state.done, 0);
    atomic_init(&io_state.code, (int)C_AsyncFileIOFailed);
    atomic_init(&io_state.bytes, -1);
    atomic_init(&close_state.done, 0);
    atomic_init(&close_state.code, (int)C_AsyncFileIOFailed);

    if (expect_status(galay_kernel_async_file_read_timeout(&runtime, &file, read_buffer,
                                                           sizeof(read_buffer), 0, 1, on_read, &io_state),
                      C_AsyncFileOperationUnsupported)) {
        return 1;
    }
    if (expect_status(galay_kernel_async_file_write_timeout(&runtime, &file, write_buffer,
                                                            sizeof(write_buffer) - 1, 0, 1, on_write, &io_state),
                      C_AsyncFileOperationUnsupported)) {
        return 2;
    }
    if (expect_status(galay_kernel_async_file_close_timeout(&runtime, &file, 1, on_close, &close_state),
                      C_AsyncFileOperationUnsupported)) {
        return 3;
    }
    if (atomic_load(&io_state.done) || atomic_load(&close_state.done)) {
        return 4;
    }
    return 0;
}
#endif

int main(void)
{
    C_AsyncFileResultCode timeout_code = C_AsyncFileTimeout;
    if (timeout_code == C_AsyncFileSuccess ||
        galay_kernel_async_file_get_error(timeout_code) == 0 ||
        strstr(galay_kernel_async_file_get_error(timeout_code), "timeout") == 0) {
        return 1;
    }

    int result = test_parameter_errors();
    if (result != 0) {
        return 10 + result;
    }

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    result = test_supported_timeout_callback();
#else
    result = test_unsupported_backend();
#endif
    if (result != 0) {
        return 100 + result;
    }

    return 0;
}
