#include <galay/c/galay-kernel-c/async-c/async_file_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

enum {
    ASYNC_FILE_DEFAULT_ITERATIONS = 256,
    ASYNC_FILE_DEFAULT_BLOCK_SIZE = 4096
};

typedef struct BenchConfig {
    int iterations;
    size_t block_size;
} BenchConfig;

typedef struct BenchState {
    const BenchConfig* config;
    galay_kernel_async_file_t* file;
    char* write_buffer;
    char* read_buffer;
    int exit_code;
    int64_t write_elapsed;
    int64_t read_elapsed;
    C_IOResult close_result;
} BenchState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static int make_temp_path(char* path, size_t length)
{
    int written = snprintf(path, length, "/tmp/galay_async_file_rw_XXXXXX");
    if (written <= 0 || (size_t)written >= length) {
        return 1;
    }

    int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    if (close(fd) != 0) {
        return 1;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        return 1;
    }
    return 0;
}

static int unlink_if_exists(const char* path)
{
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (unlink(path) == 0) {
        return 0;
    }
    return errno == ENOENT ? 0 : 1;
}

static void init_buffer(char* buffer, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        buffer[i] = (char)('a' + (i % 26));
    }
}

static void bench_entry(void* arg)
{
    BenchState* state = (BenchState*)arg;
    const int64_t write_start = now_us();
    for (int i = 0; i < state->config->iterations; ++i) {
        int64_t offset = (int64_t)i * (int64_t)state->config->block_size;
        C_IOResult written = galay_kernel_async_file_write(
            state->file,
            state->write_buffer,
            state->config->block_size,
            offset,
            1000);
        if (written.code != C_IOResultOk || written.bytes != state->config->block_size) {
            state->exit_code = 5;
            return;
        }
    }
    state->write_elapsed = now_us() - write_start;

    if (galay_kernel_async_file_sync(state->file) != C_AsyncFileSuccess) {
        state->exit_code = 6;
        return;
    }

    const int64_t read_start = now_us();
    for (int i = 0; i < state->config->iterations; ++i) {
        int64_t offset = (int64_t)i * (int64_t)state->config->block_size;
        C_IOResult read = galay_kernel_async_file_read(
            state->file,
            state->read_buffer,
            state->config->block_size,
            offset,
            1000);
        if (read.code != C_IOResultOk ||
            read.bytes != state->config->block_size ||
            memcmp(state->read_buffer, state->write_buffer, state->config->block_size) != 0) {
            state->exit_code = 7;
            return;
        }
    }
    state->read_elapsed = now_us() - read_start;
    state->close_result = galay_kernel_async_file_close(state->file, 1000);
    if (state->close_result.code != C_IOResultOk) {
        state->exit_code = 8;
        return;
    }
    state->exit_code = 0;
}

static int run_benchmark(const BenchConfig* config)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t file = {0};
    galay_coro_task_t task = {0};
    char path[64] = {0};
    char* write_buffer = NULL;
    char* read_buffer = NULL;
    int exit_code = 0;

    if (make_temp_path(path, sizeof(path)) != 0) {
        return 1;
    }

    write_buffer = (char*)malloc(config->block_size);
    read_buffer = (char*)malloc(config->block_size);
    if (write_buffer == NULL || read_buffer == NULL) {
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
        if (printf("async_file_rw unsupported: %s\n", galay_kernel_async_file_get_error(created)) < 0) {
            exit_code = 4;
        }
        goto cleanup;
    }
    if (created != C_AsyncFileSuccess ||
        galay_kernel_async_file_open(&file, path, C_AsyncFileOpenModeReadWrite, 0600) != C_AsyncFileSuccess) {
        exit_code = 5;
        goto cleanup;
    }

    BenchState state = {
        .config = config,
        .file = &file,
        .write_buffer = write_buffer,
        .read_buffer = read_buffer,
        .exit_code = 0,
        .write_elapsed = 0,
        .read_elapsed = 0,
        .close_result = {C_IOResultInvalid, 0, 0, 0, NULL},
    };
    C_IOResult spawn_result = galay_coro_spawn(&runtime, bench_entry, &state, NULL, &task);
    if (spawn_result.code != C_IOResultOk) {
        exit_code = 6;
        goto cleanup;
    }
    C_IOResult join_result = galay_coro_join(&task, 30000);
    C_IOResult destroy_result = galay_coro_destroy(&task);
    if (join_result.code != C_IOResultOk || destroy_result.code != C_IOResultOk) {
        exit_code = 7;
        goto cleanup;
    }
    if (state.exit_code != 0) {
        exit_code = state.exit_code;
        goto cleanup;
    }

    {
        const double total_mb = (double)config->iterations * (double)config->block_size / (1024.0 * 1024.0);
        const double write_seconds = state.write_elapsed > 0 ? (double)state.write_elapsed / 1000000.0 : 0.0;
        const double read_seconds = state.read_elapsed > 0 ? (double)state.read_elapsed / 1000000.0 : 0.0;
        const double write_mib = write_seconds > 0.0 ? total_mb / write_seconds : 0.0;
        const double read_mib = read_seconds > 0.0 ? total_mb / read_seconds : 0.0;
        if (printf("async_file_rw iterations=%d block_size=%zu total_mib=%.2f write_mib_s=%.2f read_mib_s=%.2f\n",
                   config->iterations,
                   config->block_size,
                   total_mb,
                   write_mib,
                   read_mib) < 0) {
            exit_code = 8;
        }
    }

cleanup:
    if (task.task != NULL) {
        if (galay_coro_join(&task, 0).code == C_IOResultOk) {
            if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
                exit_code = 9;
            }
        }
    }
    if (file.file != NULL) {
        if (galay_kernel_async_file_destroy(&file) != C_AsyncFileSuccess && exit_code == 0) {
            exit_code = 11;
        }
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 12;
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 13;
    }
    free(write_buffer);
    free(read_buffer);
    if (unlink_if_exists(path) != 0 && exit_code == 0) {
        exit_code = 14;
    }
    return exit_code;
}

static int print_usage(const char* program)
{
    return printf("Usage: %s [-n iterations] [-s block_size]\n", program) < 0 ? 1 : 0;
}

static int parse_args(int argc, char** argv, BenchConfig* config)
{
    config->iterations = ASYNC_FILE_DEFAULT_ITERATIONS;
    config->block_size = ASYNC_FILE_DEFAULT_BLOCK_SIZE;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            config->iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            config->block_size = (size_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--help") == 0) {
            return print_usage(argv[0]) == 0 ? 1 : 2;
        } else {
            return print_usage(argv[0]) == 0 ? 1 : 2;
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
