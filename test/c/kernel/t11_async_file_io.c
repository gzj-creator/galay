#include <galay/c/galay-kernel-c/async-c/async_file_c.h>

#include <stdatomic.h>
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
    atomic_store(&state->bytes, 0);
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
    atomic_store(&state->bytes, result == 0 ? 0 : (int)result->bytes);
    state->buffer = result == 0 ? 0 : result->buffer;
    state->length = result == 0 ? 0 : result->length;
    state->offset = result == 0 ? 0 : result->offset;
    atomic_store(&state->done, 1);
}

static void on_write(galay_kernel_async_file_write_result_t* result, void* ctx)
{
    IoState* state = (IoState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_AsyncFileIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? 0 : (int)result->bytes);
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
    if (length < 32) {
        return 1;
    }

    if (snprintf(path, length, "/tmp/galay_async_file_c_XXXXXX") <= 0) {
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
    galay_kernel_async_file_t file = {0};
    galay_kernel_runtime_t runtime = {0};
    char buffer[8] = {0};
    const char data[] = "x";
    size_t file_size = 0;
    IoState io_state;
    CloseState close_state;
    atomic_init(&io_state.done, 0);
    atomic_init(&io_state.code, (int)C_AsyncFileIOFailed);
    atomic_init(&io_state.bytes, 0);
    atomic_init(&close_state.done, 0);
    atomic_init(&close_state.code, (int)C_AsyncFileIOFailed);

    if (expect_status(galay_kernel_async_file_destroy(0), C_AsyncFileParameterInvalid)) {
        return 1;
    }
    if (expect_status(galay_kernel_async_file_create(0), C_AsyncFileParameterInvalid)) {
        return 2;
    }
    if (expect_status(galay_kernel_async_file_open(0, "/tmp/missing", C_AsyncFileOpenModeReadWrite, 0600), C_AsyncFileParameterInvalid)) {
        return 3;
    }
    if (expect_status(galay_kernel_async_file_size(0, &file_size), C_AsyncFileParameterInvalid)) {
        return 4;
    }
    if (expect_status(galay_kernel_async_file_size(&file, 0), C_AsyncFileParameterInvalid)) {
        return 5;
    }
    if (expect_status(galay_kernel_async_file_sync(0), C_AsyncFileParameterInvalid)) {
        return 6;
    }
    if (expect_status(galay_kernel_async_file_read(0, &file, buffer, sizeof(buffer), 0, on_read, &io_state), C_AsyncFileParameterInvalid)) {
        return 7;
    }
    if (expect_status(galay_kernel_async_file_read(&runtime, 0, buffer, sizeof(buffer), 0, on_read, &io_state), C_AsyncFileParameterInvalid)) {
        return 8;
    }
    if (expect_status(galay_kernel_async_file_read(&runtime, &file, 0, sizeof(buffer), 0, on_read, &io_state), C_AsyncFileParameterInvalid)) {
        return 9;
    }
    if (expect_status(galay_kernel_async_file_read(&runtime, &file, buffer, 0, 0, on_read, &io_state), C_AsyncFileParameterInvalid)) {
        return 10;
    }
    if (expect_status(galay_kernel_async_file_read(&runtime, &file, buffer, sizeof(buffer), 0, 0, &io_state), C_AsyncFileParameterInvalid)) {
        return 11;
    }
    if (expect_status(galay_kernel_async_file_write(0, &file, data, sizeof(data) - 1, 0, on_write, &io_state), C_AsyncFileParameterInvalid)) {
        return 12;
    }
    if (expect_status(galay_kernel_async_file_write(&runtime, 0, data, sizeof(data) - 1, 0, on_write, &io_state), C_AsyncFileParameterInvalid)) {
        return 13;
    }
    if (expect_status(galay_kernel_async_file_write(&runtime, &file, 0, sizeof(data) - 1, 0, on_write, &io_state), C_AsyncFileParameterInvalid)) {
        return 14;
    }
    if (expect_status(galay_kernel_async_file_write(&runtime, &file, data, 0, 0, on_write, &io_state), C_AsyncFileParameterInvalid)) {
        return 15;
    }
    if (expect_status(galay_kernel_async_file_write(&runtime, &file, data, sizeof(data) - 1, 0, 0, &io_state), C_AsyncFileParameterInvalid)) {
        return 16;
    }
    if (expect_status(galay_kernel_async_file_close(0, &file, on_close, &close_state), C_AsyncFileParameterInvalid)) {
        return 17;
    }
    if (expect_status(galay_kernel_async_file_close(&runtime, 0, on_close, &close_state), C_AsyncFileParameterInvalid)) {
        return 18;
    }
    if (expect_status(galay_kernel_async_file_close(&runtime, &file, 0, &close_state), C_AsyncFileParameterInvalid)) {
        return 19;
    }

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    if (expect_status(galay_kernel_async_file_create(&file), C_AsyncFileSuccess)) {
        return 20;
    }
    if (file.file == 0) {
        return 21;
    }
    if (expect_status(galay_kernel_async_file_open(&file, 0, C_AsyncFileOpenModeReadWrite, 0600), C_AsyncFileParameterInvalid)) {
        return 22;
    }
    if (expect_status(galay_kernel_async_file_open(&file, "/tmp/galay_async_file_invalid", (C_AsyncFileOpenMode)999, 0600), C_AsyncFileParameterInvalid)) {
        return 23;
    }
    if (expect_status(galay_kernel_async_file_destroy(&file), C_AsyncFileSuccess)) {
        return 24;
    }
    if (file.file != 0) {
        return 25;
    }
#else
    C_AsyncFileResultCode created = galay_kernel_async_file_create(&file);
    if (created != C_AsyncFileSuccess && created != C_AsyncFileOperationUnsupported) {
        return 20;
    }
    if (expect_status(galay_kernel_async_file_open(&file, "/tmp/galay_async_file_unsupported", C_AsyncFileOpenModeReadWrite, 0600), C_AsyncFileOperationUnsupported)) {
        return 21;
    }
    if (expect_status(galay_kernel_async_file_size(&file, &file_size), C_AsyncFileOperationUnsupported)) {
        return 22;
    }
    if (expect_status(galay_kernel_async_file_sync(&file), C_AsyncFileOperationUnsupported)) {
        return 23;
    }
    if (expect_status(galay_kernel_async_file_read(&runtime, &file, buffer, sizeof(buffer), 0, on_read, &io_state), C_AsyncFileOperationUnsupported)) {
        return 24;
    }
    if (expect_status(galay_kernel_async_file_write(&runtime, &file, data, sizeof(data) - 1, 0, on_write, &io_state), C_AsyncFileOperationUnsupported)) {
        return 25;
    }
    if (expect_status(galay_kernel_async_file_close(&runtime, &file, on_close, &close_state), C_AsyncFileOperationUnsupported)) {
        return 26;
    }
    if (expect_status(galay_kernel_async_file_destroy(&file), C_AsyncFileSuccess)) {
        return 27;
    }
#endif

    return 0;
}

#if defined(USE_KQUEUE) || defined(USE_IOURING)
static int test_stopped_runtime(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t file = {0};
    char path[64] = {0};
    char read_buffer[8] = {0};
    const char write_buffer[] = "x";
    IoState read_state;
    IoState write_state;
    CloseState close_state;
    int exit_code = 0;

    atomic_init(&read_state.done, 0);
    atomic_init(&read_state.code, (int)C_AsyncFileIOFailed);
    atomic_init(&read_state.bytes, 0);
    atomic_init(&write_state.done, 0);
    atomic_init(&write_state.code, (int)C_AsyncFileIOFailed);
    atomic_init(&write_state.bytes, 0);
    atomic_init(&close_state.done, 0);
    atomic_init(&close_state.code, (int)C_AsyncFileIOFailed);

    if (make_temp_path(path, sizeof(path)) != 0) {
        return 1;
    }
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess ||
        galay_kernel_async_file_create(&file) != C_AsyncFileSuccess ||
        galay_kernel_async_file_open(&file, path, C_AsyncFileOpenModeReadWrite, 0600) != C_AsyncFileSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    reset_io_state(&read_state);
    reset_io_state(&write_state);
    reset_close_state(&close_state);

    if (expect_status(galay_kernel_async_file_read(&runtime, &file, read_buffer, sizeof(read_buffer), 0, on_read, &read_state), C_AsyncFileRuntimeNotRunning)) {
        exit_code = 3;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_file_write(&runtime, &file, write_buffer, sizeof(write_buffer) - 1, 0, on_write, &write_state), C_AsyncFileRuntimeNotRunning)) {
        exit_code = 4;
        goto cleanup;
    }
    if (expect_status(galay_kernel_async_file_close(&runtime, &file, on_close, &close_state), C_AsyncFileRuntimeNotRunning)) {
        exit_code = 5;
        goto cleanup;
    }
    if (atomic_load(&read_state.done) || atomic_load(&write_state.done) || atomic_load(&close_state.done)) {
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

static int test_real_async_file_io(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    const char payload[] = "async-file-c-abi";
    const size_t payload_size = sizeof(payload) - 1;
    char read_buffer[64] = {0};
    char path[64] = {0};
    size_t file_size = 0;
    int exit_code = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t file = {0};
    IoState read_state;
    IoState write_state;
    CloseState close_state;

    atomic_init(&read_state.done, 0);
    atomic_init(&read_state.code, (int)C_AsyncFileIOFailed);
    atomic_init(&read_state.bytes, 0);
    atomic_init(&write_state.done, 0);
    atomic_init(&write_state.code, (int)C_AsyncFileIOFailed);
    atomic_init(&write_state.bytes, 0);
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

    if (galay_kernel_async_file_size(&file, &file_size) != C_AsyncFileSuccess || file_size != 0) {
        exit_code = 3;
        goto cleanup;
    }

    reset_io_state(&write_state);
    if (galay_kernel_async_file_write(&runtime, &file, payload, payload_size, 0, on_write, &write_state) != C_AsyncFileSuccess ||
        wait_done(&write_state.done) != 0 ||
        atomic_load(&write_state.code) != (int)C_AsyncFileSuccess ||
        atomic_load(&write_state.bytes) != (int)payload_size ||
        write_state.buffer != payload ||
        write_state.length != payload_size ||
        write_state.offset != 0) {
        exit_code = 4;
        goto cleanup;
    }

    if (galay_kernel_async_file_sync(&file) != C_AsyncFileSuccess ||
        galay_kernel_async_file_size(&file, &file_size) != C_AsyncFileSuccess ||
        file_size != payload_size) {
        exit_code = 5;
        goto cleanup;
    }

    reset_io_state(&read_state);
    if (galay_kernel_async_file_read(&runtime, &file, read_buffer, sizeof(read_buffer), 0, on_read, &read_state) != C_AsyncFileSuccess ||
        wait_done(&read_state.done) != 0 ||
        atomic_load(&read_state.code) != (int)C_AsyncFileSuccess ||
        atomic_load(&read_state.bytes) != (int)payload_size ||
        read_state.buffer != read_buffer ||
        read_state.length != sizeof(read_buffer) ||
        read_state.offset != 0 ||
        memcmp(read_buffer, payload, payload_size) != 0) {
        exit_code = 6;
        goto cleanup;
    }

    reset_close_state(&close_state);
    if (galay_kernel_async_file_close(&runtime, &file, on_close, &close_state) != C_AsyncFileSuccess ||
        wait_done(&close_state.done) != 0 ||
        atomic_load(&close_state.code) != (int)C_AsyncFileSuccess) {
        exit_code = 7;
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
#endif

int main(void)
{
    C_AsyncFileResultCode codes[] = {
        C_AsyncFileSuccess,
        C_AsyncFileParameterInvalid,
        C_AsyncFileMemoryAllocFailed,
        C_AsyncFileIOFailed,
        C_AsyncFileOperationInvalid,
        C_AsyncFileOperationUnsupported,
        C_AsyncFileRuntimeNotRunning,
        C_AsyncFileRuntimeSpawnFailed
    };
    C_AsyncFileOpenMode modes[] = {
        C_AsyncFileOpenModeRead,
        C_AsyncFileOpenModeWrite,
        C_AsyncFileOpenModeReadWrite,
        C_AsyncFileOpenModeAppend,
        C_AsyncFileOpenModeTruncate
    };

    if (codes[0] != C_AsyncFileSuccess ||
        codes[5] != C_AsyncFileOperationUnsupported ||
        modes[2] != C_AsyncFileOpenModeReadWrite ||
        galay_kernel_async_file_get_error(C_AsyncFileSuccess) == 0) {
        return 1;
    }

    int result = test_parameter_errors();
    if (result != 0) {
        return 10 + result;
    }

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    result = test_stopped_runtime();
    if (result != 0) {
        return 100 + result;
    }

    result = test_real_async_file_io();
    if (result != 0) {
        return 200 + result;
    }
#endif

    return 0;
}
