#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct TimeoutState {
    galay_kernel_file_watcher_t* watcher;
    galay_kernel_file_watcher_watch_result_t watch_result;
    C_IOResult result;
} TimeoutState;

static void watch_timeout_entry(void* arg)
{
    TimeoutState* state = (TimeoutState*)arg;
    state->result = galay_kernel_file_watcher_watch(state->watcher, &state->watch_result, 1);
}

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static int parse_iterations(int argc, char** argv)
{
    int iterations = 64;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0) {
            iterations = parsed;
        }
    }
    return iterations;
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

int main(int argc, char** argv)
{
    const int iterations = parse_iterations(argc, argv);

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_file_watcher_t watcher = {0};
    char template_path[] = "/tmp/galay-c-file-watch-timeout-bench-XXXXXX";
    int fd = mkstemp(template_path);
    int wd = -1;
    int completed = 0;
    int failures = 0;
    int exit_code = 0;

    if (fd < 0) {
        return 1;
    }
    if (close(fd) != 0) {
        return 2;
    }
    fd = -1;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        failures = 1;
        exit_code = 3;
        goto cleanup;
    }

    C_FileWatcherResultCode create_code = galay_kernel_file_watcher_create(&watcher);
    if (create_code == C_FileWatcherOperationUnsupported) {
        if (printf("file_watcher_timeout_smoke unsupported backend; skipped\n") < 0) {
            exit_code = 4;
        }
        goto cleanup;
    }
    if (create_code != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_add_watch(&watcher, template_path, C_FileWatchEventModify, &wd) !=
            C_FileWatcherSuccess) {
        failures = 1;
        exit_code = 5;
        goto cleanup;
    }

    const int64_t start = now_us();
    for (int i = 0; i < iterations; ++i) {
        TimeoutState state = {
            .watcher = &watcher,
            .watch_result = {0},
            .result = {C_IOResultInvalid, 0, 0, 0, NULL},
        };
        galay_coro_task_t task = {0};
        C_IOResult spawn_result = galay_coro_spawn(&runtime, watch_timeout_entry, &state, NULL, &task);
        if (spawn_result.code != C_IOResultOk) {
            ++failures;
            exit_code = 6;
            break;
        }
        C_IOResult join_result = galay_coro_join(&task, 2000);
        C_IOResult destroy_result = galay_coro_destroy(&task);
        if (join_result.code != C_IOResultOk ||
            destroy_result.code != C_IOResultOk ||
            state.result.code != C_IOResultTimeout ||
            state.watch_result.code != C_FileWatcherTimeout) {
            ++failures;
            exit_code = 7;
            break;
        }
        ++completed;
    }
    const int64_t elapsed = now_us() - start;

    if (printf("file_watcher_timeout_smoke iterations=%d completed=%d failures=%d elapsed_us=%lld avg_us=%.2f\n",
               iterations,
               completed,
               failures,
               (long long)elapsed,
               completed == 0 ? 0.0 : (double)elapsed / (double)completed) < 0) {
        exit_code = 8;
    }

cleanup:
    if (watcher.watcher != NULL) {
        if (wd >= 0 &&
            galay_kernel_file_watcher_remove_watch(&watcher, wd) != C_FileWatcherSuccess &&
            exit_code == 0) {
            exit_code = 9;
        }
        if (galay_kernel_file_watcher_destroy(&watcher) != C_FileWatcherSuccess && exit_code == 0) {
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
    if (fd >= 0 && close(fd) != 0 && exit_code == 0) {
        exit_code = 13;
    }
    if (unlink_if_exists(template_path) != 0 && exit_code == 0) {
        exit_code = 14;
    }
    if (exit_code != 0) {
        return exit_code;
    }
    return failures == 0 ? 0 : 15;
}
