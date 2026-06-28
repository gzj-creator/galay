#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

typedef struct WatchState {
    galay_kernel_file_watcher_t* watcher;
    C_IOResult result;
    galay_kernel_file_watcher_watch_result_t watch_result;
} WatchState;

static void watch_entry(void* ctx)
{
    WatchState* state = (WatchState*)ctx;
    state->result =
        galay_kernel_file_watcher_watch(state->watcher, &state->watch_result, 2000);
}

static int append_to_file(const char* path)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        return 1;
    }

    const char payload[] = "demo";
    int failed = write(fd, payload, sizeof(payload) - 1) != (ssize_t)(sizeof(payload) - 1);
    if (close(fd) != 0 && failed == 0) {
        failed = 1;
    }
    return failed;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_file_watcher_t watcher = {0};
    galay_coro_task_t task = {0};
    WatchState state = {&watcher, {0}, {0}};
    char template_path[] = "/tmp/galay-c-file-watch-example-XXXXXX";
    char watched_path[512] = {0};
    int fd = mkstemp(template_path);
    int wd = -1;
    int exit_code = 0;

    if (fd < 0) {
        return 1;
    }
    if (close(fd) != 0) {
        exit_code = 2;
        goto cleanup;
    }

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_file_watcher_create(&watcher) != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_add_watch(&watcher, template_path,
            (C_FileWatchEvent)(C_FileWatchEventModify | C_FileWatchEventCloseWrite), &wd) != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_get_path(&watcher, wd, watched_path, sizeof(watched_path)) != C_FileWatcherSuccess ||
        galay_coro_spawn(&runtime, watch_entry, &state, 0, &task).code != C_IOResultOk) {
        exit_code = 3;
        goto cleanup;
    }

    if (append_to_file(template_path) != 0 ||
        galay_coro_join(&task, 3000).code != C_IOResultOk ||
        state.result.code != C_IOResultOk ||
        state.watch_result.code != C_FileWatcherSuccess) {
        exit_code = 4;
        goto cleanup;
    }

    if (printf("file_watcher path=%s events=0x%x is_dir=%d\n",
               watched_path,
               (unsigned int)state.watch_result.events,
               state.watch_result.is_dir ? 1 : 0) < 0) {
        exit_code = 5;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 6;
        }
    }
    if (watcher.watcher != 0) {
        if (wd >= 0 &&
            galay_kernel_file_watcher_remove_watch(&watcher, wd) != C_FileWatcherSuccess &&
            exit_code == 0) {
            exit_code = 7;
        }
        if (galay_kernel_file_watcher_destroy(&watcher) != C_FileWatcherSuccess && exit_code == 0) {
            exit_code = 8;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 9;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 10;
        }
    }
    if (unlink(template_path) != 0 && exit_code == 0) {
        exit_code = 11;
    }
    return exit_code;
}
