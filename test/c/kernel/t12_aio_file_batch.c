#include <galay/c/galay-kernel-c/async-c/aio_file_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct CommitState {
    galay_kernel_aio_file_t* file;
    C_IOResult result;
    size_t count;
    ssize_t results[4];
} CommitState;

static int expect_status(C_AioFileResultCode actual, C_AioFileResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_io_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static void commit_entry(void* ctx)
{
    CommitState* state = (CommitState*)ctx;
    state->result = galay_kernel_aio_file_commit(state->file, state->results, 4, &state->count, -1);
}

#ifdef USE_EPOLL
static void fill_pattern(char* buffer, size_t length, char seed)
{
    for (size_t i = 0; i < length; ++i) {
        buffer[i] = (char)(seed + (char)(i % 23));
    }
}

static int test_epoll_batch_read_write(void)
{
    const size_t block_size = 4096;
    char path[] = "/tmp/galay_aio_file_c_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return 2;
    }
    if (close(fd) != 0) {
        return 2;
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

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        exit_code = 3;
        goto cleanup;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 4;
        goto cleanup;
    }
    if (expect_status(galay_kernel_aio_file_create(&file, 8), C_AioFileSuccess)) {
        exit_code = 5;
        goto cleanup;
    }
    if (expect_status(galay_kernel_aio_file_alloc_aligned_buffer(block_size, 4096, &write_a), C_AioFileSuccess) ||
        expect_status(galay_kernel_aio_file_alloc_aligned_buffer(block_size, 4096, &write_b), C_AioFileSuccess) ||
        expect_status(galay_kernel_aio_file_alloc_aligned_buffer(block_size, 4096, &read_a), C_AioFileSuccess) ||
        expect_status(galay_kernel_aio_file_alloc_aligned_buffer(block_size, 4096, &read_b), C_AioFileSuccess)) {
        exit_code = 6;
        goto cleanup;
    }
    fill_pattern(write_a, block_size, 'a');
    fill_pattern(write_b, block_size, 'K');
    if (memset(read_a, 0, block_size) != read_a ||
        memset(read_b, 0, block_size) != read_b) {
        exit_code = 35;
        goto cleanup;
    }

    if (expect_status(galay_kernel_aio_file_open(&file, path, C_AioFileOpenModeReadWrite, 0644), C_AioFileSuccess)) {
        exit_code = 7;
        goto cleanup;
    }
    if (expect_status(galay_kernel_aio_file_pre_write(&file, write_a, block_size, 0), C_AioFileSuccess) ||
        expect_status(galay_kernel_aio_file_pre_write(&file, write_b, block_size, (off_t)block_size), C_AioFileSuccess)) {
        exit_code = 8;
        goto cleanup;
    }

    galay_coro_task_t commit_task = {0};
    CommitState write_state;
    write_state.file = &file;
    write_state.result.code = C_IOResultError;
    write_state.count = 0;
    if (memset(write_state.results, 0, sizeof(write_state.results)) !=
        write_state.results) {
        exit_code = 36;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_spawn(&runtime, commit_entry, &write_state, 0, &commit_task),
            C_IOResultOk)) {
        exit_code = 9;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_join(&commit_task, 5000), C_IOResultOk)) {
        exit_code = 10;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_destroy(&commit_task), C_IOResultOk)) {
        exit_code = 18;
        goto cleanup;
    }
    if (write_state.result.code != C_IOResultOk || write_state.count != 2 ||
        write_state.results[0] != (ssize_t)block_size ||
        write_state.results[1] != (ssize_t)block_size) {
        exit_code = 11;
        goto cleanup;
    }
    if (expect_status(galay_kernel_aio_file_sync(&file), C_AioFileSuccess) ||
        expect_status(galay_kernel_aio_file_clear(&file), C_AioFileSuccess)) {
        exit_code = 12;
        goto cleanup;
    }

    size_t file_size = 0;
    if (expect_status(galay_kernel_aio_file_size(&file, &file_size), C_AioFileSuccess) ||
        file_size < block_size * 2) {
        exit_code = 13;
        goto cleanup;
    }
    if (expect_status(galay_kernel_aio_file_pre_read(&file, read_a, block_size, 0), C_AioFileSuccess) ||
        expect_status(galay_kernel_aio_file_pre_read(&file, read_b, block_size, (off_t)block_size), C_AioFileSuccess)) {
        exit_code = 14;
        goto cleanup;
    }

    CommitState read_state;
    read_state.file = &file;
    read_state.result.code = C_IOResultError;
    read_state.count = 0;
    if (memset(read_state.results, 0, sizeof(read_state.results)) !=
        read_state.results) {
        exit_code = 37;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_spawn(&runtime, commit_entry, &read_state, 0, &commit_task),
            C_IOResultOk)) {
        exit_code = 15;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_join(&commit_task, 5000), C_IOResultOk)) {
        exit_code = 16;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_destroy(&commit_task), C_IOResultOk)) {
        exit_code = 19;
        goto cleanup;
    }
    if (read_state.result.code != C_IOResultOk || read_state.count != 2 ||
        read_state.results[0] != (ssize_t)block_size ||
        read_state.results[1] != (ssize_t)block_size ||
        memcmp(write_a, read_a, block_size) != 0 ||
        memcmp(write_b, read_b, block_size) != 0) {
        exit_code = 17;
        goto cleanup;
    }

cleanup:
    if (commit_task.task != 0) {
        if (galay_coro_destroy(&commit_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 26;
        }
    }
    if (file.file != 0) {
        if (galay_kernel_aio_file_close(&file) != C_AioFileSuccess && exit_code == 0) {
            exit_code = 27;
        }
        if (galay_kernel_aio_file_destroy(&file) != C_AioFileSuccess && exit_code == 0) {
            exit_code = 28;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 29;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 30;
        }
    }
    if (write_a != 0) {
        if (galay_kernel_aio_file_free_aligned_buffer(write_a) != C_AioFileSuccess &&
            exit_code == 0) {
            exit_code = 31;
        }
    }
    if (write_b != 0) {
        if (galay_kernel_aio_file_free_aligned_buffer(write_b) != C_AioFileSuccess &&
            exit_code == 0) {
            exit_code = 32;
        }
    }
    if (read_a != 0) {
        if (galay_kernel_aio_file_free_aligned_buffer(read_a) != C_AioFileSuccess &&
            exit_code == 0) {
            exit_code = 33;
        }
    }
    if (read_b != 0) {
        if (galay_kernel_aio_file_free_aligned_buffer(read_b) != C_AioFileSuccess &&
            exit_code == 0) {
            exit_code = 34;
        }
    }
    if (unlink(path) != 0 && errno != ENOENT && exit_code == 0) {
        exit_code = 38;
    }
    return exit_code;
}
#else
static int test_unsupported_backend(void)
{
    galay_kernel_aio_file_t file = {0};
    galay_kernel_aio_file_t fake_file = {(void*)1};
    galay_kernel_runtime_t fake_runtime = {(void*)1};
    char buffer[512] = {0};
    char* aligned = 0;
    size_t file_size = 0;
    size_t count = 0;
    CommitState state;
    state.file = &fake_file;
    state.result.code = C_IOResultError;
    state.count = 0;

    if (expect_status(galay_kernel_aio_file_create(0, 4), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_create(&file, 0), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_destroy(0), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_open(0, "/tmp/unused", C_AioFileOpenModeReadWrite, 0644), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_open(&file, "/tmp/unused", C_AioFileOpenModeReadWrite, 0644), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_open(&fake_file, "/tmp/unused", (C_AioFileOpenMode)99, 0644), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_pre_read(0, buffer, sizeof(buffer), 0), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_pre_read(&file, buffer, sizeof(buffer), 0), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_pre_read(&fake_file, 0, sizeof(buffer), 0), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_pre_write(0, buffer, sizeof(buffer), 0), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_pre_write(&file, buffer, sizeof(buffer), 0), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_pre_write(&fake_file, 0, sizeof(buffer), 0), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_clear(0), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_clear(&file), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_close(0), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_close(&file), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_size(0, &file_size), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_size(&file, &file_size), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_size(&fake_file, 0), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_sync(0), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_sync(&file), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_alloc_aligned_buffer(0, 512, &aligned), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_alloc_aligned_buffer(512, 512, 0), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_alloc_aligned_buffer(512, 3, &aligned), C_AioFileParameterInvalid) ||
        expect_io_code(galay_kernel_aio_file_commit(0, state.results, 4, &count, -1), C_IOResultInvalid) ||
        expect_io_code(galay_kernel_aio_file_commit(&fake_file, 0, 4, &count, -1), C_IOResultInvalid) ||
        expect_io_code(galay_kernel_aio_file_commit(&fake_file, state.results, 0, &count, -1), C_IOResultInvalid) ||
        expect_io_code(galay_kernel_aio_file_commit(&fake_file, state.results, 4, 0, -1), C_IOResultInvalid)) {
        return 19;
    }
    if (expect_status(galay_kernel_aio_file_create(&file, 4), C_AioFileOperationUnsupported)) {
        return 20;
    }
    if (file.file != 0) {
        return 21;
    }
    if (expect_status(galay_kernel_aio_file_alloc_aligned_buffer(512, 512, &aligned), C_AioFileOperationUnsupported)) {
        return 22;
    }
    if (aligned != 0) {
        return 23;
    }
    if (expect_status(galay_kernel_aio_file_open(&fake_file, "/tmp/unused", C_AioFileOpenModeReadWrite, 0644), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_pre_read(&fake_file, buffer, sizeof(buffer), 0), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_pre_write(&fake_file, buffer, sizeof(buffer), 0), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_clear(&fake_file), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_size(&fake_file, &file_size), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_sync(&fake_file), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_close(&fake_file), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_destroy(&fake_file), C_AioFileOperationUnsupported)) {
        return 24;
    }
    fake_file.file = (void*)1;
    if (expect_io_code(galay_kernel_aio_file_commit(&fake_file, state.results, 4, &count, -1), C_IOResultError)) {
        return 25;
    }
    if (galay_kernel_aio_file_get_error(C_AioFileOperationUnsupported) == 0) {
        return 27;
    }
    return 0;
}
#endif

int main(void)
{
#ifdef USE_EPOLL
    return test_epoll_batch_read_write();
#else
    return test_unsupported_backend();
#endif
}
