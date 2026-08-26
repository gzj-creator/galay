#include <galay/c/galay-kernel-c/async-c/file_watcher.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct WatchState {
    galay_c_file_watcher_t* watcher;
    C_IOResult result;
    galay_c_file_event_t event;
} WatchState;

static int expect_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static void watch_entry(void* arg)
{
    WatchState* const state = arg;
    state->result = galay_c_file_watcher_wait(state->watcher, &state->event, 1000);
}

static int append_byte(const char* path)
{
    const int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        return 1;
    }
    int failed = write(fd, "x", 1) != 1;
    if (fsync(fd) != 0) {
        failed = 1;
    }
    if (close(fd) != 0) {
        failed = 1;
    }
    return failed;
}

int main(void)
{
    galay_c_file_watcher_t invalid = {.fd = -1};
    galay_c_file_event_t event = {0};
    if (expect_code(galay_c_file_watcher_create(NULL), C_IOResultInvalid) ||
        expect_code(galay_c_file_watcher_add_watch(NULL, "/tmp", GALAY_C_WATCH_MODIFY),
                    C_IOResultInvalid) ||
        expect_code(galay_c_file_watcher_add_watch(&invalid, NULL, GALAY_C_WATCH_MODIFY),
                    C_IOResultInvalid) ||
        expect_code(galay_c_file_watcher_wait(NULL, &event, 0), C_IOResultInvalid) ||
        expect_code(galay_c_file_watcher_wait(&invalid, NULL, 0), C_IOResultInvalid) ||
        expect_code(galay_c_file_watcher_close(NULL), C_IOResultInvalid) ||
        expect_code(galay_c_file_watcher_close(&invalid), C_IOResultOk)) {
        return 1;
    }

    char path[] = "/tmp/galay-c-file-watch-XXXXXX";
    const int temp_fd = mkstemp(path);
    if (temp_fd < 0) {
        return 2;
    }
    if (close(temp_fd) != 0) {
        return 3;
    }

    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.parallel_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_file_watcher_t watcher = {.fd = -1};
    galay_c_coro_task_t task = {0};
    int result = 0;
    int watch_descriptor = -1;

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

    WatchState state = {.watcher = &watcher};
    if (galay_c_coro_spawn(&runtime, watch_entry, &state, NULL, &task).code != C_IOResultOk ||
        append_byte(path) != 0 ||
        galay_c_coro_join(&task, 2000).code != C_IOResultOk) {
        result = 7;
    } else if (state.result.code != C_IOResultOk ||
               (state.event.mask & GALAY_C_WATCH_MODIFY) == 0 || state.event.is_dir) {
        result = 8;
    }

cleanup:
    if (task.task != NULL && galay_c_coro_destroy(&task).code != C_IOResultOk && result == 0) {
        result = 9;
    }
    if (watch_descriptor >= 0 && watcher.fd >= 0) {
        const C_IOResult removed =
            galay_c_file_watcher_remove_watch(&watcher, watch_descriptor);
        if (removed.code != C_IOResultOk && result == 0) {
            result = 10;
        }
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
