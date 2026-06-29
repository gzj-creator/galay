#include <galay/c/galay-kernel-c/async-c/aio_file_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef USE_EPOLL
#include <stdlib.h>
#endif

#ifndef USE_EPOLL
int main(void)
{
    galay_kernel_aio_file_t file = {0};
    C_AioFileResultCode code = galay_kernel_aio_file_create(&file, 4);
    if (code == C_AioFileOperationUnsupported) {
        return printf("aio_file_batch unsupported backend: %s\n",
                      galay_kernel_aio_file_get_error(code)) < 0 ? 1 : 0;
    }
    return printf("aio_file_batch unexpected result: %s\n",
                  galay_kernel_aio_file_get_error(code)) < 0 ? 2 : 1;
}
#else
typedef struct CommitState {
    galay_kernel_aio_file_t* file;
    C_IOResult result;
    size_t count;
    ssize_t results[2];
} CommitState;

static void commit_entry(void* ctx)
{
    CommitState* state = (CommitState*)ctx;
    state->result = galay_kernel_aio_file_commit(state->file, state->results, 2, &state->count, -1);
}

static void init_state(CommitState* state, galay_kernel_aio_file_t* file)
{
    state->file = file;
    state->result = (C_IOResult){C_IOResultError, 0, 0, 0, 0};
    state->count = 0;
    state->results[0] = 0;
    state->results[1] = 0;
}

static void fill_pattern(char* buffer, size_t length, char seed)
{
    for (size_t i = 0; i < length; ++i) {
        buffer[i] = (char)(seed + (char)(i % 17));
    }
}

static void zero_buffer(char* buffer, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        buffer[i] = 0;
    }
}

static int commit_and_join(galay_kernel_runtime_t* runtime, CommitState* state)
{
    galay_coro_task_t task = {0};
    C_IOResult spawned = galay_coro_spawn(runtime, commit_entry, state, 0, &task);
    if (spawned.code != C_IOResultOk) {
        return 1;
    }
    C_IOResult joined = galay_coro_join(&task, 5000);
    if (joined.code != C_IOResultOk) {
        C_IOResult destroyed = galay_coro_destroy(&task);
        return destroyed.code == C_IOResultOk ? 2 : 3;
    }
    C_IOResult destroyed = galay_coro_destroy(&task);
    if (destroyed.code != C_IOResultOk) {
        return 4;
    }
    return state->result.code == C_IOResultOk && state->count == 2 ? 0 : 5;
}

int main(void)
{
    const size_t block_size = 4096;
    char path[] = "/tmp/galay_aio_file_example_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    if (close(fd) != 0) {
        return 1;
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_aio_file_t file = {0};
    char* write_a = 0;
    char* write_b = 0;
    char* read_a = 0;
    char* read_b = 0;
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_aio_file_create(&file, 8) != C_AioFileSuccess ||
        galay_kernel_aio_file_alloc_aligned_buffer(block_size, 4096, &write_a) != C_AioFileSuccess ||
        galay_kernel_aio_file_alloc_aligned_buffer(block_size, 4096, &write_b) != C_AioFileSuccess ||
        galay_kernel_aio_file_alloc_aligned_buffer(block_size, 4096, &read_a) != C_AioFileSuccess ||
        galay_kernel_aio_file_alloc_aligned_buffer(block_size, 4096, &read_b) != C_AioFileSuccess ||
        galay_kernel_aio_file_open(&file, path, C_AioFileOpenModeReadWrite, 0644) != C_AioFileSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    fill_pattern(write_a, block_size, 'A');
    fill_pattern(write_b, block_size, 'k');
    zero_buffer(read_a, block_size);
    zero_buffer(read_b, block_size);

    if (galay_kernel_aio_file_pre_write(&file, write_a, block_size, 0) != C_AioFileSuccess ||
        galay_kernel_aio_file_pre_write(&file, write_b, block_size, (off_t)block_size) != C_AioFileSuccess) {
        exit_code = 3;
        goto cleanup;
    }

    CommitState write_state;
    init_state(&write_state, &file);
    if (commit_and_join(&runtime, &write_state) != 0 ||
        write_state.results[0] != (ssize_t)block_size ||
        write_state.results[1] != (ssize_t)block_size) {
        exit_code = 4;
        goto cleanup;
    }
    if (galay_kernel_aio_file_sync(&file) != C_AioFileSuccess ||
        galay_kernel_aio_file_clear(&file) != C_AioFileSuccess) {
        exit_code = 5;
        goto cleanup;
    }

    if (galay_kernel_aio_file_pre_read(&file, read_a, block_size, 0) != C_AioFileSuccess ||
        galay_kernel_aio_file_pre_read(&file, read_b, block_size, (off_t)block_size) != C_AioFileSuccess) {
        exit_code = 6;
        goto cleanup;
    }

    CommitState read_state;
    init_state(&read_state, &file);
    if (commit_and_join(&runtime, &read_state) != 0 ||
        read_state.results[0] != (ssize_t)block_size ||
        read_state.results[1] != (ssize_t)block_size ||
        memcmp(write_a, read_a, block_size) != 0 ||
        memcmp(write_b, read_b, block_size) != 0) {
        exit_code = 7;
        goto cleanup;
    }

    size_t file_size = 0;
    if (galay_kernel_aio_file_size(&file, &file_size) != C_AioFileSuccess) {
        exit_code = 8;
        goto cleanup;
    }

    if (printf("aio_file_batch path=%s file_size=%zu write_results=%zd,%zd read_results=%zd,%zd\n",
               path,
               file_size,
               write_state.results[0],
               write_state.results[1],
               read_state.results[0],
               read_state.results[1]) < 0) {
        exit_code = 18;
    }

cleanup:
    if (file.file != 0) {
        if (galay_kernel_aio_file_close(&file) != C_AioFileSuccess && exit_code == 0) {
            exit_code = 9;
        }
        if (galay_kernel_aio_file_destroy(&file) != C_AioFileSuccess && exit_code == 0) {
            exit_code = 10;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 11;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 12;
        }
    }
    if (write_a != 0) {
        if (galay_kernel_aio_file_free_aligned_buffer(write_a) != C_AioFileSuccess &&
            exit_code == 0) {
            exit_code = 13;
        }
    }
    if (write_b != 0) {
        if (galay_kernel_aio_file_free_aligned_buffer(write_b) != C_AioFileSuccess &&
            exit_code == 0) {
            exit_code = 14;
        }
    }
    if (read_a != 0) {
        if (galay_kernel_aio_file_free_aligned_buffer(read_a) != C_AioFileSuccess &&
            exit_code == 0) {
            exit_code = 15;
        }
    }
    if (read_b != 0) {
        if (galay_kernel_aio_file_free_aligned_buffer(read_b) != C_AioFileSuccess &&
            exit_code == 0) {
            exit_code = 16;
        }
    }
    if (unlink(path) != 0 && errno != ENOENT && exit_code == 0) {
        exit_code = 17;
    }
    return exit_code;
}
#endif
