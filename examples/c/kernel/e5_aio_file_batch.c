#include <galay/c/galay-kernel-c/async-c/aio_file_c.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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
        printf("aio_file_batch unsupported backend: %s\n", galay_kernel_aio_file_get_error(code));
        return 0;
    }
    printf("aio_file_batch unexpected result: %s\n", galay_kernel_aio_file_get_error(code));
    return 1;
}
#else
typedef struct CommitState {
    atomic_int done;
    C_AioFileResultCode code;
    size_t count;
    ssize_t results[2];
} CommitState;

static void on_commit(galay_kernel_aio_file_commit_result_t* result, void* ctx)
{
    CommitState* state = (CommitState*)ctx;
    state->code = result->code;
    state->count = result->count;
    for (size_t i = 0; i < result->count && i < 2; ++i) {
        state->results[i] = result->results[i];
    }
    atomic_store(&state->done, 1);
}

static int wait_commit(CommitState* state)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 5000; ++i) {
        if (atomic_load(&state->done)) {
            return state->code == C_AioFileSuccess ? 0 : 1;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static void init_state(CommitState* state)
{
    atomic_init(&state->done, 0);
    state->code = C_AioFileIOFailed;
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

int main(void)
{
    const size_t block_size = 4096;
    char path[] = "/tmp/galay_aio_file_example_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    close(fd);

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
    memset(read_a, 0, block_size);
    memset(read_b, 0, block_size);

    if (galay_kernel_aio_file_pre_write(&file, write_a, block_size, 0) != C_AioFileSuccess ||
        galay_kernel_aio_file_pre_write(&file, write_b, block_size, (off_t)block_size) != C_AioFileSuccess) {
        exit_code = 3;
        goto cleanup;
    }

    CommitState write_state;
    init_state(&write_state);
    if (galay_kernel_aio_file_commit(&runtime, &file, on_commit, &write_state) != C_AioFileSuccess ||
        wait_commit(&write_state) != 0 ||
        write_state.count != 2) {
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
    init_state(&read_state);
    if (galay_kernel_aio_file_commit(&runtime, &file, on_commit, &read_state) != C_AioFileSuccess ||
        wait_commit(&read_state) != 0 ||
        read_state.count != 2 ||
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

    printf("aio_file_batch path=%s file_size=%zu write_results=%zd,%zd read_results=%zd,%zd\n",
           path,
           file_size,
           write_state.results[0],
           write_state.results[1],
           read_state.results[0],
           read_state.results[1]);

cleanup:
    if (file.file != 0) {
        (void)galay_kernel_aio_file_close(&file);
        (void)galay_kernel_aio_file_destroy(&file);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    if (write_a != 0) {
        (void)galay_kernel_aio_file_free_aligned_buffer(write_a);
    }
    if (write_b != 0) {
        (void)galay_kernel_aio_file_free_aligned_buffer(write_b);
    }
    if (read_a != 0) {
        (void)galay_kernel_aio_file_free_aligned_buffer(read_a);
    }
    if (read_b != 0) {
        (void)galay_kernel_aio_file_free_aligned_buffer(read_b);
    }
    unlink(path);
    return exit_code;
}
#endif
