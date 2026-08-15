#include <galay/c/galay-kernel-c/async-c/file_watcher.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct WatchState {
    galay_c_file_watcher_t* watcher;
    C_IOResult result;
    galay_c_file_event_t event;
} WatchState;

static void watch_entry(void* arg)
{
    WatchState* const state = arg;
    state->result = galay_c_file_watcher_wait(state->watcher, &state->event, 2000);
}

static int append_to_file(const char* path)
{
    const int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        return 1;
    }
    int failed = write(fd, "demo", 4) != 4;
    if (close(fd) != 0) {
        failed = 1;
    }
    return failed;
}

int main(void)
{
    char path[] = "/tmp/galay-c-file-watch-example-XXXXXX";
    const int temp_fd = mkstemp(path);
    if (temp_fd < 0 || close(temp_fd) != 0) {
        return 1;
    }

    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_file_watcher_t watcher = {.fd = -1};
    galay_c_coro_task_t task = {0};
    WatchState state = {.watcher = &watcher};
    int watch_descriptor = -1;
    int result = 0;

    if (galay_c_file_watcher_create(&watcher).code != C_IOResultOk) {
        result = 2;
        goto cleanup;
    }
    const C_IOResult added =
        galay_c_file_watcher_add_watch(&watcher, path, GALAY_C_WATCH_MODIFY);
    if (added.code != C_IOResultOk) {
        result = 3;
        goto cleanup;
    }
    watch_descriptor = (int)added.value;
    if (galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_c_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_c_coro_spawn(&runtime, watch_entry, &state, NULL, &task).code !=
            C_IOResultOk ||
        append_to_file(path) != 0 ||
        galay_c_coro_join(&task, 3000).code != C_IOResultOk ||
        state.result.code != C_IOResultOk) {
        result = 4;
        goto cleanup;
    }
    if (printf("file_watcher path=%s events=0x%x is_dir=%d\n",
               path, (unsigned int)state.event.mask, state.event.is_dir) < 0) {
        result = 5;
    }

cleanup:
    if (task.task != NULL && galay_c_coro_destroy(&task).code != C_IOResultOk && result == 0) {
        result = 6;
    }
    if (watch_descriptor >= 0 && watcher.fd >= 0 &&
        galay_c_file_watcher_remove_watch(&watcher, watch_descriptor).code != C_IOResultOk &&
        result == 0) {
        result = 7;
    }
    if (watcher.fd >= 0 && galay_c_file_watcher_close(&watcher).code != C_IOResultOk &&
        result == 0) {
        result = 8;
    }
    if (runtime.runtime != NULL) {
        if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 9;
        }
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 10;
        }
    }
    if (unlink(path) != 0 && errno != ENOENT && result == 0) {
        result = 11;
    }
    return result;
}
