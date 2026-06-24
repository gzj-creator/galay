#include <galay/c/galay-kernel-c/async-c/async_file_c.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    COPY_BUFFER_SIZE = 4096
};

typedef struct IoState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
} IoState;

typedef struct CloseState {
    atomic_int done;
    atomic_int code;
} CloseState;

static void reset_io_state(IoState* state)
{
    atomic_store(&state->done, 0);
    atomic_store(&state->code, (int)C_AsyncFileIOFailed);
    atomic_store(&state->bytes, 0);
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
    atomic_store(&state->done, 1);
}

static void on_write(galay_kernel_async_file_write_result_t* result, void* ctx)
{
    IoState* state = (IoState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_AsyncFileIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? 0 : (int)result->bytes);
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
    for (int i = 0; i < 5000; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static int make_demo_source(char* path, size_t length)
{
    const char content[] = "galay async file C ABI copy example\n";

    if (snprintf(path, length, "/tmp/galay_async_file_copy_src_XXXXXX") <= 0) {
        return 1;
    }

    int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }

    size_t written = 0;
    while (written < sizeof(content) - 1) {
        ssize_t chunk = write(fd, content + written, sizeof(content) - 1 - written);
        if (chunk <= 0) {
            close(fd);
            return 1;
        }
        written += (size_t)chunk;
    }

    close(fd);
    return 0;
}

static int close_async_file(galay_kernel_runtime_t* runtime, galay_kernel_async_file_t* file)
{
    CloseState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_AsyncFileIOFailed);
    reset_close_state(&state);

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

static int copy_file(const char* source_path, const char* dest_path)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t source = {0};
    galay_kernel_async_file_t dest = {0};
    char buffer[COPY_BUFFER_SIZE];
    size_t offset = 0;
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        return 1;
    }

    C_AsyncFileResultCode created = galay_kernel_async_file_create(&source);
    if (created == C_AsyncFileOperationUnsupported) {
        printf("async_file_copy unsupported: %s\n", galay_kernel_async_file_get_error(created));
        goto cleanup;
    }
    if (created != C_AsyncFileSuccess ||
        galay_kernel_async_file_create(&dest) != C_AsyncFileSuccess ||
        galay_kernel_async_file_open(&source, source_path, C_AsyncFileOpenModeRead, 0600) != C_AsyncFileSuccess ||
        galay_kernel_async_file_open(&dest, dest_path, C_AsyncFileOpenModeTruncate, 0600) != C_AsyncFileSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    while (1) {
        IoState read_state;
        atomic_init(&read_state.done, 0);
        atomic_init(&read_state.code, (int)C_AsyncFileIOFailed);
        atomic_init(&read_state.bytes, 0);
        reset_io_state(&read_state);

        if (galay_kernel_async_file_read(&runtime, &source, buffer, sizeof(buffer), offset, on_read, &read_state) != C_AsyncFileSuccess ||
            wait_done(&read_state.done) != 0 ||
            atomic_load(&read_state.code) != (int)C_AsyncFileSuccess) {
            exit_code = 3;
            goto cleanup;
        }

        int read_bytes = atomic_load(&read_state.bytes);
        if (read_bytes == 0) {
            break;
        }

        IoState write_state;
        atomic_init(&write_state.done, 0);
        atomic_init(&write_state.code, (int)C_AsyncFileIOFailed);
        atomic_init(&write_state.bytes, 0);
        reset_io_state(&write_state);

        if (galay_kernel_async_file_write(&runtime, &dest, buffer, (size_t)read_bytes, offset, on_write, &write_state) != C_AsyncFileSuccess ||
            wait_done(&write_state.done) != 0 ||
            atomic_load(&write_state.code) != (int)C_AsyncFileSuccess ||
            atomic_load(&write_state.bytes) != read_bytes) {
            exit_code = 4;
            goto cleanup;
        }

        offset += (size_t)read_bytes;
    }

    if (galay_kernel_async_file_sync(&dest) != C_AsyncFileSuccess) {
        exit_code = 5;
        goto cleanup;
    }

    printf("async_file_copy source=%s dest=%s bytes=%zu\n", source_path, dest_path, offset);

cleanup:
    if (source.file != 0) {
        (void)close_async_file(&runtime, &source);
        (void)galay_kernel_async_file_destroy(&source);
    }
    if (dest.file != 0) {
        (void)close_async_file(&runtime, &dest);
        (void)galay_kernel_async_file_destroy(&dest);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}

int main(int argc, char** argv)
{
    char source_path[64] = {0};
    char dest_path[64] = {0};
    int remove_demo_source = 0;
    int remove_demo_dest = 0;

    const char* source = 0;
    const char* dest = 0;
    if (argc == 3) {
        source = argv[1];
        dest = argv[2];
    } else {
        if (make_demo_source(source_path, sizeof(source_path)) != 0 ||
            snprintf(dest_path, sizeof(dest_path), "/tmp/galay_async_file_copy_dst_%ld", (long)getpid()) <= 0) {
            return 1;
        }
        source = source_path;
        dest = dest_path;
        remove_demo_source = 1;
        remove_demo_dest = 1;
    }

    int result = copy_file(source, dest);
    if (remove_demo_source) {
        unlink(source_path);
    }
    if (remove_demo_dest) {
        unlink(dest_path);
    }
    return result;
}
