#include <galay/c/galay-kernel-c/async-c/file_watcher.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct WatchTimeoutState {
    galay_c_file_watcher_t* watcher;
    C_IOResult result;
    galay_c_file_event_t event;
} WatchTimeoutState;

static void watch_timeout_entry(void* arg)
{
    WatchTimeoutState* const state = arg;
    state->result = galay_c_file_watcher_wait(state->watcher, &state->event, 20);
}

int main(void)
{
    galay_c_file_watcher_t invalid = {.fd = -1};
    galay_c_file_event_t invalid_event = {0};
    if (galay_c_file_watcher_wait(NULL, &invalid_event, 1).code != C_IOResultInvalid ||
        galay_c_file_watcher_wait(&invalid, NULL, 1).code != C_IOResultInvalid ||
        galay_c_file_watcher_wait(&invalid, &invalid_event, 1).code != C_IOResultInvalid) {
        return 1;
    }

    char path[] = "/tmp/galay-c-file-watch-timeout-XXXXXX";
    const int temp_fd = mkstemp(path);
    if (temp_fd < 0) {
        return 2;
    }
    if (close(temp_fd) != 0) {
        return 3;
    }

    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_file_watcher_t watcher = {.fd = -1};
    galay_c_coro_task_t task = {0};
    int watch_descriptor = -1;
    int result = 0;

    if (galay_c_file_watcher_create(&watcher).code != C_IOResultOk) {
        result = 4;
        goto cleanup;
    }
    const C_IOResult added =
        galay_c_file_watcher_add_watch(&watcher, path, GALAY_C_WATCH_MODIFY);
    if (added.code != C_IOResultOk) {
        result = 5;
        goto cleanup;
    }
    watch_descriptor = (int)added.value;
    if (galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_c_runtime_start(&runtime) != C_RuntimeSuccess) {
        result = 6;
        goto cleanup;
    }

    WatchTimeoutState state = {.watcher = &watcher};
    if (galay_c_coro_spawn(&runtime, watch_timeout_entry, &state, NULL, &task).code !=
            C_IOResultOk ||
        galay_c_coro_join(&task, 2000).code != C_IOResultOk) {
        result = 7;
    } else if (state.result.code != C_IOResultTimeout || state.event.mask != 0 ||
               state.event.name[0] != '\0') {
        result = 8;
    }

cleanup:
    if (task.task != NULL && galay_c_coro_destroy(&task).code != C_IOResultOk && result == 0) {
        result = 9;
    }
    if (watch_descriptor >= 0 && watcher.fd >= 0 &&
        galay_c_file_watcher_remove_watch(&watcher, watch_descriptor).code != C_IOResultOk &&
        result == 0) {
        result = 10;
    }
    if (watcher.fd >= 0 && galay_c_file_watcher_close(&watcher).code != C_IOResultOk &&
        result == 0) {
        result = 11;
    }
    if (runtime.runtime != NULL) {
        if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 12;
        }
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 13;
        }
    }
    if (unlink(path) != 0 && errno != ENOENT && result == 0) {
        result = 14;
    }
    return result;
}
