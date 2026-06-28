#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct WatchState {
    atomic_int done;
    galay_kernel_file_watcher_t* watcher;
    C_IOResult io_result;
    galay_kernel_file_watcher_watch_result_t watch_result;
} WatchState;

static int expect_status(C_FileWatcherResultCode actual, C_FileWatcherResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_io_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static void watch_entry(void* ctx)
{
    WatchState* state = (WatchState*)ctx;
    state->io_result =
        galay_kernel_file_watcher_watch(state->watcher, &state->watch_result, 1000);
    atomic_store(&state->done, 1);
}

static int wait_done(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        if (nanosleep(&pause, 0) != 0 && errno != EINTR) {
            return 1;
        }
    }
    return 1;
}

static int append_to_file(const char* path)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        return 1;
    }

    const char payload[] = "x";
    int failed = write(fd, payload, sizeof(payload) - 1) != (ssize_t)(sizeof(payload) - 1);
    if (fsync(fd) != 0) {
        failed = 1;
    }
    if (close(fd) != 0) {
        failed = 1;
    }
    return failed;
}

static int trigger_until_done(const char* path, atomic_int* done)
{
    struct timespec pause = {0, 10000000};
    for (int i = 0; i < 200 && !atomic_load(done); ++i) {
        if (append_to_file(path) != 0) {
            return 1;
        }
        if (nanosleep(&pause, 0) != 0 && errno != EINTR) {
            return 1;
        }
    }
    return wait_done(done);
}

static void init_state(WatchState* state)
{
    atomic_init(&state->done, 0);
    state->watcher = 0;
    state->io_result = (C_IOResult){0};
    state->watch_result = (galay_kernel_file_watcher_watch_result_t){0};
}

static int test_parameter_errors(void)
{
    galay_kernel_file_watcher_t watcher = {0};
    WatchState state;
    int wd = -1;
    char path_buffer[4] = {0};
    char template_path[] = "/tmp/galay-c-file-watch-param-XXXXXX";
    int fd = mkstemp(template_path);
    int exit_code = 0;

    if (fd < 0) {
        return 10;
    }
    if (close(fd) != 0) {
        return 10;
    }
    init_state(&state);

    if (expect_status(galay_kernel_file_watcher_create(0), C_FileWatcherParameterInvalid)) {
        exit_code = 11;
        goto cleanup_file;
    }
    if (expect_status(galay_kernel_file_watcher_destroy(0), C_FileWatcherParameterInvalid)) {
        exit_code = 12;
        goto cleanup_file;
    }
    if (expect_status(galay_kernel_file_watcher_add_watch(0, template_path, C_FileWatchEventModify, &wd),
            C_FileWatcherParameterInvalid)) {
        exit_code = 13;
        goto cleanup_file;
    }
    if (expect_status(galay_kernel_file_watcher_get_path(0, 1, path_buffer, sizeof(path_buffer)),
            C_FileWatcherParameterInvalid)) {
        exit_code = 14;
        goto cleanup_file;
    }
    if (expect_io_code(galay_kernel_file_watcher_watch(0, &state.watch_result, -1),
            C_IOResultInvalid)) {
        exit_code = 15;
        goto cleanup_file;
    }
    if (expect_status(galay_kernel_file_watcher_create(&watcher), C_FileWatcherSuccess)) {
        exit_code = 16;
        goto cleanup_file;
    }
    if (expect_status(galay_kernel_file_watcher_add_watch(&watcher, 0, C_FileWatchEventModify, &wd),
            C_FileWatcherParameterInvalid)) {
        exit_code = 17;
        goto cleanup;
    }
    if (expect_status(galay_kernel_file_watcher_add_watch(&watcher, template_path, C_FileWatchEventModify, 0),
            C_FileWatcherParameterInvalid)) {
        exit_code = 18;
        goto cleanup;
    }
    if (expect_status(galay_kernel_file_watcher_add_watch(&watcher, template_path, C_FileWatchEventModify, &wd),
            C_FileWatcherSuccess)) {
        exit_code = 19;
        goto cleanup;
    }
    if (expect_status(galay_kernel_file_watcher_get_path(&watcher, wd, 0, sizeof(path_buffer)),
            C_FileWatcherParameterInvalid)) {
        exit_code = 20;
        goto cleanup;
    }
    if (expect_status(galay_kernel_file_watcher_get_path(&watcher, wd, path_buffer, 0),
            C_FileWatcherParameterInvalid)) {
        exit_code = 21;
        goto cleanup;
    }
    if (expect_status(galay_kernel_file_watcher_get_path(&watcher, wd, path_buffer, sizeof(path_buffer)),
            C_FileWatcherParameterInvalid)) {
        exit_code = 22;
        goto cleanup;
    }
    if (expect_io_code(galay_kernel_file_watcher_watch(&watcher, 0, -1),
            C_IOResultInvalid)) {
        exit_code = 23;
        goto cleanup;
    }
    if (expect_io_code(galay_kernel_file_watcher_watch(&watcher, &state.watch_result, -1),
            C_IOResultInvalid)) {
        exit_code = 24;
        goto cleanup;
    }

cleanup:
    if (watcher.watcher != 0) {
        if (galay_kernel_file_watcher_destroy(&watcher) != C_FileWatcherSuccess &&
            exit_code == 0) {
            exit_code = 25;
        }
    }
cleanup_file:
    if (unlink(template_path) != 0 && errno != ENOENT && exit_code == 0) {
        exit_code = 26;
    }
    return exit_code;
}

static int test_file_modify_event(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_file_watcher_t watcher = {0};
    galay_coro_task_t watch_task = {0};
    WatchState state;
    char template_path[] = "/tmp/galay-c-file-watch-event-XXXXXX";
    char copied_path[512] = {0};
    int fd = mkstemp(template_path);
    int wd = -1;
    int exit_code = 0;

    if (fd < 0) {
        return 40;
    }
    if (close(fd) != 0) {
        return 40;
    }
    init_state(&state);
    state.watcher = &watcher;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_file_watcher_create(&watcher) != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_add_watch(&watcher, template_path,
            (C_FileWatchEvent)(C_FileWatchEventModify | C_FileWatchEventCloseWrite), &wd) != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_get_path(&watcher, wd, copied_path, sizeof(copied_path)) != C_FileWatcherSuccess ||
        strcmp(copied_path, template_path) != 0) {
        exit_code = 41;
        goto cleanup;
    }

    if (expect_io_code(galay_coro_spawn(&runtime, watch_entry, &state, 0, &watch_task),
            C_IOResultOk) ||
        trigger_until_done(template_path, &state.done) != 0) {
        exit_code = 42;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_join(&watch_task, 2000), C_IOResultOk)) {
        exit_code = 43;
        goto cleanup;
    }

    if (state.io_result.code != C_IOResultOk ||
        state.watch_result.code != C_FileWatcherSuccess ||
        (state.watch_result.events &
            (C_FileWatchEvent)(C_FileWatchEventModify | C_FileWatchEventCloseWrite)) == 0 ||
        state.watch_result.is_dir != 0) {
        exit_code = 44;
        goto cleanup;
    }

cleanup:
    if (watch_task.task != 0) {
        if (galay_coro_destroy(&watch_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 45;
        }
    }
    if (watcher.watcher != 0) {
        if (wd >= 0) {
            if (galay_kernel_file_watcher_remove_watch(&watcher, wd) !=
                    C_FileWatcherSuccess &&
                exit_code == 0) {
                exit_code = 46;
            }
        }
        if (galay_kernel_file_watcher_destroy(&watcher) != C_FileWatcherSuccess &&
            exit_code == 0) {
            exit_code = 47;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 48;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 49;
        }
    }
    if (unlink(template_path) != 0 && errno != ENOENT && exit_code == 0) {
        exit_code = 50;
    }
    return exit_code;
}

int main(void)
{
    int result = test_parameter_errors();
    if (result != 0) {
        return result;
    }
    return test_file_modify_event();
}
