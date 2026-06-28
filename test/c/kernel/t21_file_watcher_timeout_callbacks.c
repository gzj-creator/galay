#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct WatchTimeoutState {
    galay_kernel_file_watcher_t* watcher;
    C_IOResult io_result;
    galay_kernel_file_watcher_watch_result_t watch_result;
} WatchTimeoutState;

static int expect_status(C_FileWatcherResultCode actual, C_FileWatcherResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_io_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static int create_started_runtime(galay_kernel_runtime_t* runtime)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    if (galay_kernel_runtime_create(&config, runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(runtime) != C_RuntimeSuccess) {
        if (galay_kernel_runtime_destroy(runtime) != C_RuntimeSuccess) {
            return 2;
        }
        return 1;
    }
    return 0;
}

static void watch_timeout_entry(void* ctx)
{
    WatchTimeoutState* state = (WatchTimeoutState*)ctx;
    state->io_result =
        galay_kernel_file_watcher_watch(state->watcher, &state->watch_result, 20);
}

static int test_timeout_api_parameter_errors(void)
{
    galay_kernel_file_watcher_t watcher = {0};
    galay_kernel_file_watcher_watch_result_t result = {0};

    if (expect_status(C_FileWatcherTimeout, C_FileWatcherTimeout)) {
        return 10;
    }
    if (strcmp(galay_kernel_file_watcher_get_error(C_FileWatcherTimeout), "timeout") != 0) {
        return 11;
    }
    if (expect_io_code(galay_kernel_file_watcher_watch(0, &result, 1), C_IOResultInvalid)) {
        return 12;
    }
    if (expect_io_code(galay_kernel_file_watcher_watch(&watcher, 0, 1), C_IOResultInvalid)) {
        return 13;
    }
    if (expect_io_code(galay_kernel_file_watcher_watch(&watcher, &result, 1), C_IOResultInvalid)) {
        return 14;
    }
    return 0;
}

static int test_watch_timeout_without_file_event(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_file_watcher_t watcher = {0};
    galay_coro_task_t watch_task = {0};
    WatchTimeoutState state = {0};
    char template_path[] = "/tmp/galay-c-file-watch-timeout-XXXXXX";
    int fd = mkstemp(template_path);
    int wd = -1;
    int exit_code = 0;

    if (fd < 0) {
        return 20;
    }
    if (close(fd) != 0) {
        return 20;
    }
    state.watcher = &watcher;

    if (create_started_runtime(&runtime) != 0) {
        exit_code = 21;
        goto cleanup_file;
    }

    C_FileWatcherResultCode create_code = galay_kernel_file_watcher_create(&watcher);
    if (create_code == C_FileWatcherOperationUnsupported) {
        C_IOResult watch_code = galay_kernel_file_watcher_watch(&watcher, &state.watch_result, 1);
        if (watch_code.code != C_IOResultInvalid) {
            exit_code = 22;
        } else {
            if (printf("file watcher timeout unsupported backend; skipped event timeout check\n") < 0) {
                exit_code = 33;
            }
        }
        goto cleanup_runtime;
    }
    if (create_code != C_FileWatcherSuccess) {
        exit_code = 23;
        goto cleanup_runtime;
    }

    if (galay_kernel_file_watcher_add_watch(&watcher, template_path, C_FileWatchEventModify, &wd) !=
        C_FileWatcherSuccess) {
        exit_code = 24;
        goto cleanup_watcher;
    }

    if (expect_io_code(galay_coro_spawn(&runtime, watch_timeout_entry, &state, 0, &watch_task),
            C_IOResultOk) ||
        expect_io_code(galay_coro_join(&watch_task, 2000), C_IOResultOk)) {
        exit_code = 25;
        goto cleanup_watcher;
    }
    if (state.io_result.code != C_IOResultTimeout ||
        state.watch_result.code != C_FileWatcherTimeout ||
        state.watch_result.events != C_FileWatchEventNone ||
        state.watch_result.name[0] != '\0') {
        exit_code = 26;
        goto cleanup_watcher;
    }

cleanup_watcher:
    if (watch_task.task != 0) {
        if (galay_coro_destroy(&watch_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 27;
        }
    }
    if (watcher.watcher != 0) {
        if (wd >= 0) {
            if (galay_kernel_file_watcher_remove_watch(&watcher, wd) !=
                    C_FileWatcherSuccess &&
                exit_code == 0) {
                exit_code = 28;
            }
        }
        if (galay_kernel_file_watcher_destroy(&watcher) != C_FileWatcherSuccess &&
            exit_code == 0) {
            exit_code = 29;
        }
    }
cleanup_runtime:
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
        exit_code = 30;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
        exit_code = 31;
    }
cleanup_file:
    if (unlink(template_path) != 0 && errno != ENOENT && exit_code == 0) {
        exit_code = 32;
    }
    return exit_code;
}

int main(void)
{
    int result = test_timeout_api_parameter_errors();
    if (result != 0) {
        return result;
    }
    return test_watch_timeout_without_file_event();
}
