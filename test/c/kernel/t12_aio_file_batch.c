#include <galay/c/galay-kernel-c/async-c/aio_file_c.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct CommitState {
    atomic_int done;
    C_AioFileResultCode code;
    size_t count;
    ssize_t results[4];
} CommitState;

static int expect_status(C_AioFileResultCode actual, C_AioFileResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static void on_commit(galay_kernel_aio_file_commit_result_t* result, void* ctx)
{
    CommitState* state = (CommitState*)ctx;
    state->code = result->code;
    state->count = result->count;
    for (size_t i = 0; i < result->count && i < 4; ++i) {
        state->results[i] = result->results[i];
    }
    atomic_store(&state->done, 1);
}

static int wait_for_commit(CommitState* state)
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
    memset(read_a, 0, block_size);
    memset(read_b, 0, block_size);

    if (expect_status(galay_kernel_aio_file_open(&file, path, C_AioFileOpenModeReadWrite, 0644), C_AioFileSuccess)) {
        exit_code = 7;
        goto cleanup;
    }
    if (expect_status(galay_kernel_aio_file_pre_write(&file, write_a, block_size, 0), C_AioFileSuccess) ||
        expect_status(galay_kernel_aio_file_pre_write(&file, write_b, block_size, (off_t)block_size), C_AioFileSuccess)) {
        exit_code = 8;
        goto cleanup;
    }

    CommitState write_state;
    atomic_init(&write_state.done, 0);
    write_state.code = C_AioFileIOFailed;
    write_state.count = 0;
    memset(write_state.results, 0, sizeof(write_state.results));
    if (expect_status(galay_kernel_aio_file_commit(&runtime, &file, on_commit, &write_state), C_AioFileSuccess)) {
        exit_code = 9;
        goto cleanup;
    }
    if (wait_for_commit(&write_state) || write_state.count != 2 ||
        write_state.results[0] != (ssize_t)block_size ||
        write_state.results[1] != (ssize_t)block_size) {
        exit_code = 10;
        goto cleanup;
    }
    if (expect_status(galay_kernel_aio_file_sync(&file), C_AioFileSuccess) ||
        expect_status(galay_kernel_aio_file_clear(&file), C_AioFileSuccess)) {
        exit_code = 11;
        goto cleanup;
    }

    size_t file_size = 0;
    if (expect_status(galay_kernel_aio_file_size(&file, &file_size), C_AioFileSuccess) ||
        file_size < block_size * 2) {
        exit_code = 12;
        goto cleanup;
    }
    if (expect_status(galay_kernel_aio_file_pre_read(&file, read_a, block_size, 0), C_AioFileSuccess) ||
        expect_status(galay_kernel_aio_file_pre_read(&file, read_b, block_size, (off_t)block_size), C_AioFileSuccess)) {
        exit_code = 13;
        goto cleanup;
    }

    CommitState read_state;
    atomic_init(&read_state.done, 0);
    read_state.code = C_AioFileIOFailed;
    read_state.count = 0;
    memset(read_state.results, 0, sizeof(read_state.results));
    if (expect_status(galay_kernel_aio_file_commit(&runtime, &file, on_commit, &read_state), C_AioFileSuccess)) {
        exit_code = 14;
        goto cleanup;
    }
    if (wait_for_commit(&read_state) || read_state.count != 2 ||
        read_state.results[0] != (ssize_t)block_size ||
        read_state.results[1] != (ssize_t)block_size ||
        memcmp(write_a, read_a, block_size) != 0 ||
        memcmp(write_b, read_b, block_size) != 0) {
        exit_code = 15;
        goto cleanup;
    }

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
#else
static int test_unsupported_backend(void)
{
    galay_kernel_aio_file_t file = {0};
    galay_kernel_aio_file_t fake_file = {(void*)1};
    galay_kernel_runtime_t fake_runtime = {(void*)1};
    char buffer[512] = {0};
    char* aligned = 0;
    size_t file_size = 0;
    CommitState state;
    atomic_init(&state.done, 0);
    state.code = C_AioFileIOFailed;
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
        expect_status(galay_kernel_aio_file_commit(0, &fake_file, on_commit, &state), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_commit(&fake_runtime, 0, on_commit, &state), C_AioFileParameterInvalid) ||
        expect_status(galay_kernel_aio_file_commit(&fake_runtime, &file, on_commit, &state), C_AioFileOperationUnsupported) ||
        expect_status(galay_kernel_aio_file_commit(&fake_runtime, &fake_file, 0, &state), C_AioFileParameterInvalid)) {
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
    if (expect_status(galay_kernel_aio_file_commit(&fake_runtime, &fake_file, on_commit, &state), C_AioFileOperationUnsupported)) {
        return 25;
    }
    if (atomic_load(&state.done) != 0) {
        return 26;
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
