#include <galay/c/galay-kernel-c/async-c/async_file_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum {
    COPY_BUFFER_SIZE = 4096
};

typedef struct CopyState {
    galay_kernel_async_file_t* source;
    galay_kernel_async_file_t* dest;
    C_IOResult read_result;
    C_IOResult write_result;
    C_IOResult close_source_result;
    C_IOResult close_dest_result;
    size_t copied;
    char buffer[COPY_BUFFER_SIZE];
} CopyState;

static void copy_entry(void* ctx)
{
    CopyState* state = (CopyState*)ctx;
    size_t offset = 0;
    for (;;) {
        state->read_result =
            galay_kernel_async_file_read(state->source, state->buffer, sizeof(state->buffer), (int64_t)offset, 2000);
        if (state->read_result.code == C_IOResultEof ||
            (state->read_result.code == C_IOResultOk && state->read_result.bytes == 0)) {
            state->read_result.code = C_IOResultOk;
            break;
        }
        if (state->read_result.code != C_IOResultOk) {
            return;
        }
        state->write_result =
            galay_kernel_async_file_write(state->dest,
                                          state->buffer,
                                          state->read_result.bytes,
                                          (int64_t)offset,
                                          2000);
        if (state->write_result.code != C_IOResultOk ||
            state->write_result.bytes != state->read_result.bytes) {
            return;
        }
        offset += state->read_result.bytes;
        state->copied = offset;
    }
    state->close_source_result = galay_kernel_async_file_close(state->source, 2000);
    state->close_dest_result = galay_kernel_async_file_close(state->dest, 2000);
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
            int close_result = close(fd);
            return close_result == 0 ? 1 : 2;
        }
        written += (size_t)chunk;
    }

    return close(fd) == 0 ? 0 : 3;
}

static int copy_file(const char* source_path, const char* dest_path)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t source = {0};
    galay_kernel_async_file_t dest = {0};
    galay_coro_task_t task = {0};
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        return 1;
    }

    C_AsyncFileResultCode created = galay_kernel_async_file_create(&source);
    if (created == C_AsyncFileOperationUnsupported) {
        if (printf("async_file_copy unsupported: %s\n", galay_kernel_async_file_get_error(created)) < 0) {
            exit_code = 2;
        }
        goto cleanup;
    }
    if (created != C_AsyncFileSuccess ||
        galay_kernel_async_file_create(&dest) != C_AsyncFileSuccess ||
        galay_kernel_async_file_open(&source, source_path, C_AsyncFileOpenModeRead, 0600) != C_AsyncFileSuccess ||
        galay_kernel_async_file_open(&dest, dest_path, C_AsyncFileOpenModeTruncate, 0600) != C_AsyncFileSuccess) {
        exit_code = 3;
        goto cleanup;
    }

    CopyState state = {0};
    state.source = &source;
    state.dest = &dest;
    if (galay_coro_spawn(&runtime, copy_entry, &state, 0, &task).code != C_IOResultOk ||
        galay_coro_join(&task, 5000).code != C_IOResultOk) {
        exit_code = 4;
        goto cleanup;
    }
    if (state.read_result.code != C_IOResultOk ||
        state.write_result.code != C_IOResultOk ||
        state.close_source_result.code != C_IOResultOk ||
        state.close_dest_result.code != C_IOResultOk) {
        exit_code = 5;
        goto cleanup;
    }

    if (printf("async_file_copy source=%s dest=%s bytes=%zu\n", source_path, dest_path, state.copied) < 0) {
        exit_code = 6;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 7;
        }
    }
    if (source.file != 0) {
        if (galay_kernel_async_file_destroy(&source) != C_AsyncFileSuccess && exit_code == 0) {
            exit_code = 8;
        }
    }
    if (dest.file != 0) {
        if (galay_kernel_async_file_destroy(&dest) != C_AsyncFileSuccess && exit_code == 0) {
            exit_code = 9;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 10;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 11;
        }
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
    if (remove_demo_source && unlink(source_path) != 0 && result == 0) {
        result = 12;
    }
    if (remove_demo_dest && unlink(dest_path) != 0 && result == 0) {
        result = 13;
    }
    return result;
}
