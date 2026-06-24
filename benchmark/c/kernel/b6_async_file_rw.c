#include <galay/c/galay-kernel-c/async-c/async_file_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum {
    ASYNC_FILE_DEFAULT_ITERATIONS = 256,
    ASYNC_FILE_DEFAULT_BLOCK_SIZE = 4096
};

typedef struct BenchConfig {
    int iterations;
    size_t block_size;
} BenchConfig;

typedef struct IoState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
} IoState;

typedef struct CloseState {
    atomic_int done;
    atomic_int code;
} CloseState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

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

static int make_temp_path(char* path, size_t length)
{
    if (snprintf(path, length, "/tmp/galay_async_file_rw_XXXXXX") <= 0) {
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

static void init_buffer(char* buffer, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        buffer[i] = (char)('a' + (i % 26));
    }
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

static int run_benchmark(const BenchConfig* config)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t file = {0};
    char path[64] = {0};
    char* write_buffer = 0;
    char* read_buffer = 0;
    int exit_code = 0;

    if (make_temp_path(path, sizeof(path)) != 0) {
        return 1;
    }

    write_buffer = (char*)malloc(config->block_size);
    read_buffer = (char*)malloc(config->block_size);
    if (write_buffer == 0 || read_buffer == 0) {
        exit_code = 2;
        goto cleanup;
    }
    init_buffer(write_buffer, config->block_size);

    if (galay_kernel_runtime_create(&runtime_config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 3;
        goto cleanup;
    }

    C_AsyncFileResultCode created = galay_kernel_async_file_create(&file);
    if (created == C_AsyncFileOperationUnsupported) {
        printf("async_file_rw unsupported: %s\n", galay_kernel_async_file_get_error(created));
        goto cleanup;
    }
    if (created != C_AsyncFileSuccess ||
        galay_kernel_async_file_open(&file, path, C_AsyncFileOpenModeReadWrite, 0600) != C_AsyncFileSuccess) {
        exit_code = 4;
        goto cleanup;
    }

    const int64_t write_start = now_us();
    for (int i = 0; i < config->iterations; ++i) {
        IoState state;
        atomic_init(&state.done, 0);
        atomic_init(&state.code, (int)C_AsyncFileIOFailed);
        atomic_init(&state.bytes, 0);
        reset_io_state(&state);

        size_t offset = (size_t)i * config->block_size;
        if (galay_kernel_async_file_write(&runtime, &file, write_buffer, config->block_size, offset, on_write, &state) != C_AsyncFileSuccess ||
            wait_done(&state.done) != 0 ||
            atomic_load(&state.code) != (int)C_AsyncFileSuccess ||
            atomic_load(&state.bytes) != (int)config->block_size) {
            exit_code = 5;
            goto cleanup;
        }
    }
    const int64_t write_elapsed = now_us() - write_start;

    if (galay_kernel_async_file_sync(&file) != C_AsyncFileSuccess) {
        exit_code = 6;
        goto cleanup;
    }

    const int64_t read_start = now_us();
    for (int i = 0; i < config->iterations; ++i) {
        IoState state;
        atomic_init(&state.done, 0);
        atomic_init(&state.code, (int)C_AsyncFileIOFailed);
        atomic_init(&state.bytes, 0);
        reset_io_state(&state);
        memset(read_buffer, 0, config->block_size);

        size_t offset = (size_t)i * config->block_size;
        if (galay_kernel_async_file_read(&runtime, &file, read_buffer, config->block_size, offset, on_read, &state) != C_AsyncFileSuccess ||
            wait_done(&state.done) != 0 ||
            atomic_load(&state.code) != (int)C_AsyncFileSuccess ||
            atomic_load(&state.bytes) != (int)config->block_size ||
            memcmp(read_buffer, write_buffer, config->block_size) != 0) {
            exit_code = 7;
            goto cleanup;
        }
    }
    const int64_t read_elapsed = now_us() - read_start;

    {
        const double total_mb = (double)config->iterations * (double)config->block_size / (1024.0 * 1024.0);
        const double write_seconds = write_elapsed > 0 ? (double)write_elapsed / 1000000.0 : 0.0;
        const double read_seconds = read_elapsed > 0 ? (double)read_elapsed / 1000000.0 : 0.0;
        const double write_mib = write_seconds > 0.0 ? total_mb / write_seconds : 0.0;
        const double read_mib = read_seconds > 0.0 ? total_mb / read_seconds : 0.0;
        printf("async_file_rw iterations=%d block_size=%zu total_mib=%.2f write_mib_s=%.2f read_mib_s=%.2f\n",
               config->iterations,
               config->block_size,
               total_mb,
               write_mib,
               read_mib);
    }

cleanup:
    if (file.file != 0) {
        (void)close_async_file(&runtime, &file);
        (void)galay_kernel_async_file_destroy(&file);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    free(write_buffer);
    free(read_buffer);
    unlink(path);
    return exit_code;
}

static void print_usage(const char* program)
{
    printf("Usage: %s [-n iterations] [-s block_size]\n", program);
}

static int parse_args(int argc, char** argv, BenchConfig* config)
{
    config->iterations = ASYNC_FILE_DEFAULT_ITERATIONS;
    config->block_size = ASYNC_FILE_DEFAULT_BLOCK_SIZE;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            config->iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            config->block_size = (size_t)strtoull(argv[++i], 0, 10);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    return config->iterations <= 0 || config->block_size == 0;
}

int main(int argc, char** argv)
{
    BenchConfig config;
    if (parse_args(argc, argv, &config) != 0) {
        return 1;
    }
    return run_benchmark(&config);
}
