#include <galay/c/galay-kernel-c/async-c/async_file_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct FileOpState {
    galay_kernel_async_file_t* file;
    const char* payload;
    size_t payload_size;
    char* read_buffer;
    size_t read_buffer_size;
    C_IOResult result;
    C_AsyncFileResultCode sync_result;
} FileOpState;

static void write_entry(void* arg)
{
    FileOpState* state = (FileOpState*)arg;
    state->result = galay_kernel_async_file_write(
        state->file,
        state->payload,
        state->payload_size,
        0,
        1000);
}

static void read_entry(void* arg)
{
    FileOpState* state = (FileOpState*)arg;
    state->result = galay_kernel_async_file_read(
        state->file,
        state->read_buffer,
        state->read_buffer_size,
        0,
        1000);
}

static void close_entry(void* arg)
{
    FileOpState* state = (FileOpState*)arg;
    state->result = galay_kernel_async_file_close(state->file, 1000);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int make_temp_path(char* path, size_t length)
{
    int written = snprintf(path, length, "/tmp/galay_async_file_timeout_smoke_XXXXXX");
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

static int run_file_task(galay_kernel_runtime_t* runtime,
                         galay_coro_entry_fn entry,
                         FileOpState* state,
                         int64_t join_timeout_ms)
{
    galay_coro_task_t task = {0};
    C_IOResult spawn_result = galay_coro_spawn(runtime, entry, state, NULL, &task);
    if (spawn_result.code != C_IOResultOk) {
        return 1;
    }
    C_IOResult join_result = galay_coro_join(&task, join_timeout_ms);
    C_IOResult destroy_result = galay_coro_destroy(&task);
    return join_result.code == C_IOResultOk && destroy_result.code == C_IOResultOk ? 0 : 1;
}

static int parse_iterations(int argc, char** argv)
{
    int iterations = 32;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0) {
            iterations = parsed;
        }
    }
    return iterations;
}

int main(int argc, char** argv)
{
    const int iterations = parse_iterations(argc, argv);

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_async_file_t file = {0};
    char path[64] = {0};
    char read_buffer[8] = {0};
    const char payload[] = "galay";
    const size_t payload_size = sizeof(payload) - 1;
    int completed = 0;
    int failures = 0;
    int exit_code = 0;

    if (make_temp_path(path, sizeof(path)) != 0 ||
        galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        failures = 1;
        exit_code = 1;
        goto cleanup;
    }

    C_AsyncFileResultCode created = galay_kernel_async_file_create(&file);
    if (created == C_AsyncFileOperationUnsupported) {
        if (printf("async_file_timeout_smoke unsupported: %s\n",
                   galay_kernel_async_file_get_error(created)) < 0) {
            exit_code = 2;
        }
        completed = iterations;
        goto cleanup;
    }
    if (created != C_AsyncFileSuccess ||
        galay_kernel_async_file_open(&file, path, C_AsyncFileOpenModeReadWrite, 0600) !=
            C_AsyncFileSuccess) {
        failures = 1;
        exit_code = 3;
        goto cleanup;
    }

    FileOpState write_state = {
        .file = &file,
        .payload = payload,
        .payload_size = payload_size,
        .read_buffer = NULL,
        .read_buffer_size = 0,
        .result = {C_IOResultInvalid, 0, 0, 0, NULL},
        .sync_result = C_AsyncFileIOFailed,
    };
    if (run_file_task(&runtime, write_entry, &write_state, 3000) != 0 ||
        write_state.result.code != C_IOResultOk ||
        write_state.result.bytes != payload_size ||
        galay_kernel_async_file_sync(&file) != C_AsyncFileSuccess) {
        failures = 1;
        exit_code = 4;
        goto cleanup;
    }

    const uint64_t start_ns = now_ns();
    for (int i = 0; i < iterations; ++i) {
        FileOpState read_state = {
            .file = &file,
            .payload = NULL,
            .payload_size = 0,
            .read_buffer = read_buffer,
            .read_buffer_size = sizeof(read_buffer),
            .result = {C_IOResultInvalid, 0, 0, 0, NULL},
            .sync_result = C_AsyncFileIOFailed,
        };
        if (run_file_task(&runtime, read_entry, &read_state, 3000) != 0 ||
            read_state.result.code != C_IOResultOk ||
            read_state.result.bytes != payload_size ||
            memcmp(read_buffer, payload, payload_size) != 0) {
            if (printf("async_file_timeout_smoke read failed: code=%d bytes=%zu\n",
                       (int)read_state.result.code,
                       read_state.result.bytes) < 0) {
                exit_code = 5;
            } else {
                exit_code = 6;
            }
            ++failures;
            break;
        }
        ++completed;
    }
    const uint64_t elapsed_ns = now_ns() - start_ns;

    if (printf("async_file_timeout_smoke iterations=%d completed=%d failures=%d elapsed_ns=%llu avg_ns=%.2f\n",
               iterations,
               completed,
               failures,
               (unsigned long long)elapsed_ns,
               completed == 0 ? 0.0 : (double)elapsed_ns / (double)completed) < 0) {
        exit_code = 7;
    }

cleanup:
    if (file.file != NULL) {
        FileOpState close_state = {
            .file = &file,
            .payload = NULL,
            .payload_size = 0,
            .read_buffer = NULL,
            .read_buffer_size = 0,
            .result = {C_IOResultInvalid, 0, 0, 0, NULL},
            .sync_result = C_AsyncFileIOFailed,
        };
        if (runtime.runtime != NULL &&
            run_file_task(&runtime, close_entry, &close_state, 3000) != 0 &&
            exit_code == 0) {
            exit_code = 8;
        }
        if (runtime.runtime != NULL &&
            close_state.result.code != C_IOResultOk &&
            exit_code == 0) {
            exit_code = 9;
        }
        if (galay_kernel_async_file_destroy(&file) != C_AsyncFileSuccess && exit_code == 0) {
            exit_code = 10;
        }
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 11;
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 12;
    }
    if (unlink_if_exists(path) != 0 && exit_code == 0) {
        exit_code = 13;
    }
    if (exit_code != 0) {
        return exit_code;
    }
    return failures == 0 && completed == iterations ? 0 : 14;
}
