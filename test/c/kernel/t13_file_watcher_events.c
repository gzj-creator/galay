#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct WatchState {
    atomic_int done;
    atomic_int code;
    atomic_uint events;
    atomic_int is_dir;
    char name[256];
} WatchState;

static int expect_status(C_FileWatcherResultCode actual, C_FileWatcherResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static void on_watch(galay_kernel_file_watcher_watch_result_t* result, void* ctx)
{
    WatchState* state = (WatchState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_FileWatcherIOFailed : (int)result->code);
    atomic_store(&state->events, result == 0 ? 0u : (unsigned int)result->events);
    atomic_store(&state->is_dir, result == 0 ? 0 : result->is_dir);
    if (result != 0) {
        memcpy(state->name, result->name, sizeof(state->name));
        state->name[sizeof(state->name) - 1] = '\0';
    }
    atomic_store(&state->done, 1);
}

static int wait_done(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        nanosleep(&pause, 0);
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
    (void)fsync(fd);
    close(fd);
    return failed;
}

static int trigger_until_done(const char* path, atomic_int* done)
{
    struct timespec pause = {0, 10000000};
    for (int i = 0; i < 200 && !atomic_load(done); ++i) {
        if (append_to_file(path) != 0) {
            return 1;
        }
        nanosleep(&pause, 0);
    }
    return wait_done(done);
}

static void init_state(WatchState* state)
{
    atomic_init(&state->done, 0);
    atomic_init(&state->code, (int)C_FileWatcherIOFailed);
    atomic_init(&state->events, 0u);
    atomic_init(&state->is_dir, 0);
    memset(state->name, 0, sizeof(state->name));
}

static int test_parameter_errors(void)
{
    galay_kernel_file_watcher_t watcher = {0};
    int wd = -1;
    char path_buffer[4] = {0};
    char template_path[] = "/tmp/galay-c-file-watch-param-XXXXXX";
    int fd = mkstemp(template_path);
    int exit_code = 0;

    if (fd < 0) {
        return 10;
    }
    close(fd);

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
    if (expect_status(galay_kernel_file_watcher_watch(0, &watcher, on_watch, 0),
            C_FileWatcherParameterInvalid)) {
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

cleanup:
    if (watcher.watcher != 0) {
        (void)galay_kernel_file_watcher_destroy(&watcher);
    }
cleanup_file:
    unlink(template_path);
    return exit_code;
}

static int test_stopped_runtime(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_file_watcher_t watcher = {0};
    WatchState state;
    char template_path[] = "/tmp/galay-c-file-watch-stopped-XXXXXX";
    int fd = mkstemp(template_path);
    int wd = -1;
    int exit_code = 0;

    if (fd < 0) {
        return 30;
    }
    close(fd);
    init_state(&state);

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess ||
        galay_kernel_file_watcher_create(&watcher) != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_add_watch(&watcher, template_path, C_FileWatchEventModify, &wd) != C_FileWatcherSuccess) {
        exit_code = 31;
        goto cleanup;
    }

    if (expect_status(galay_kernel_file_watcher_watch(&runtime, &watcher, on_watch, &state),
            C_FileWatcherRuntimeNotRunning)) {
        exit_code = 32;
        goto cleanup;
    }
    if (atomic_load(&state.done) != 0) {
        exit_code = 33;
        goto cleanup;
    }

cleanup:
    if (watcher.watcher != 0) {
        (void)galay_kernel_file_watcher_destroy(&watcher);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    unlink(template_path);
    return exit_code;
}

static int test_file_modify_event(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_file_watcher_t watcher = {0};
    WatchState state;
    char template_path[] = "/tmp/galay-c-file-watch-event-XXXXXX";
    char copied_path[512] = {0};
    int fd = mkstemp(template_path);
    int wd = -1;
    int exit_code = 0;

    if (fd < 0) {
        return 40;
    }
    close(fd);
    init_state(&state);

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

    if (galay_kernel_file_watcher_watch(&runtime, &watcher, on_watch, &state) != C_FileWatcherSuccess ||
        trigger_until_done(template_path, &state.done) != 0) {
        exit_code = 42;
        goto cleanup;
    }

    if (atomic_load(&state.code) != (int)C_FileWatcherSuccess ||
        (atomic_load(&state.events) & (unsigned int)(C_FileWatchEventModify | C_FileWatchEventCloseWrite)) == 0 ||
        atomic_load(&state.is_dir) != 0) {
        exit_code = 43;
        goto cleanup;
    }

cleanup:
    if (watcher.watcher != 0) {
        if (wd >= 0) {
            (void)galay_kernel_file_watcher_remove_watch(&watcher, wd);
        }
        (void)galay_kernel_file_watcher_destroy(&watcher);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    unlink(template_path);
    return exit_code;
}

int main(void)
{
    int result = test_parameter_errors();
    if (result != 0) {
        return result;
    }
    result = test_stopped_runtime();
    if (result != 0) {
        return result;
    }
    return test_file_modify_event();
}
