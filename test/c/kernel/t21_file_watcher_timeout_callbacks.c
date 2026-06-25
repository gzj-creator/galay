#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>

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
    char name[256];
} WatchState;

static int expect_status(C_FileWatcherResultCode actual, C_FileWatcherResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static void init_state(WatchState* state)
{
    atomic_init(&state->done, 0);
    atomic_init(&state->code, (int)C_FileWatcherIOFailed);
    atomic_init(&state->events, 0u);
    memset(state->name, 0, sizeof(state->name));
}

static void on_watch_timeout(galay_kernel_file_watcher_watch_result_t* result, void* ctx)
{
    WatchState* state = (WatchState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_FileWatcherIOFailed : (int)result->code);
    atomic_store(&state->events, result == 0 ? 0u : (unsigned int)result->events);
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

static int create_started_runtime(galay_kernel_runtime_t* runtime)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    if (galay_kernel_runtime_create(&config, runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(runtime) != C_RuntimeSuccess) {
        (void)galay_kernel_runtime_destroy(runtime);
        return 1;
    }
    return 0;
}

static int test_timeout_api_parameter_errors(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_file_watcher_t watcher = {0};
    WatchState state;
    int exit_code = 0;

    init_state(&state);

    if (expect_status(C_FileWatcherTimeout, C_FileWatcherTimeout)) {
        return 10;
    }
    if (strcmp(galay_kernel_file_watcher_get_error(C_FileWatcherTimeout), "timeout") != 0) {
        return 11;
    }
    if (expect_status(galay_kernel_file_watcher_watch_timeout(0, &watcher, 1, on_watch_timeout, &state),
            C_FileWatcherParameterInvalid)) {
        return 12;
    }

    if (create_started_runtime(&runtime) != 0) {
        return 13;
    }
    if (expect_status(galay_kernel_file_watcher_watch_timeout(&runtime, 0, 1, on_watch_timeout, &state),
            C_FileWatcherParameterInvalid)) {
        exit_code = 14;
        goto cleanup;
    }
    if (expect_status(galay_kernel_file_watcher_watch_timeout(&runtime, &watcher, 1, 0, &state),
            C_FileWatcherParameterInvalid)) {
        exit_code = 15;
        goto cleanup;
    }
    if (atomic_load(&state.done) != 0) {
        exit_code = 16;
        goto cleanup;
    }

cleanup:
    (void)galay_kernel_runtime_stop(&runtime);
    (void)galay_kernel_runtime_destroy(&runtime);
    return exit_code;
}

static int test_watch_timeout_without_file_event(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_file_watcher_t watcher = {0};
    WatchState state;
    char template_path[] = "/tmp/galay-c-file-watch-timeout-XXXXXX";
    int fd = mkstemp(template_path);
    int wd = -1;
    int exit_code = 0;

    if (fd < 0) {
        return 20;
    }
    close(fd);
    init_state(&state);

    if (create_started_runtime(&runtime) != 0) {
        exit_code = 21;
        goto cleanup_file;
    }

    C_FileWatcherResultCode create_code = galay_kernel_file_watcher_create(&watcher);
    if (create_code == C_FileWatcherOperationUnsupported) {
        C_FileWatcherResultCode watch_code =
            galay_kernel_file_watcher_watch_timeout(&runtime, &watcher, 1, on_watch_timeout, &state);
        if (watch_code != C_FileWatcherOperationUnsupported || atomic_load(&state.done) != 0) {
            exit_code = 22;
        } else {
            printf("file watcher timeout unsupported backend; skipped event timeout check\n");
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

    if (galay_kernel_file_watcher_watch_timeout(&runtime, &watcher, 20, on_watch_timeout, &state) !=
        C_FileWatcherSuccess) {
        exit_code = 25;
        goto cleanup_watcher;
    }
    if (wait_done(&state.done) != 0) {
        exit_code = 26;
        goto cleanup_watcher;
    }
    if (atomic_load(&state.code) != (int)C_FileWatcherTimeout ||
        atomic_load(&state.events) != 0u ||
        state.name[0] != '\0') {
        exit_code = 27;
        goto cleanup_watcher;
    }

cleanup_watcher:
    if (watcher.watcher != 0) {
        if (wd >= 0) {
            (void)galay_kernel_file_watcher_remove_watch(&watcher, wd);
        }
        (void)galay_kernel_file_watcher_destroy(&watcher);
    }
cleanup_runtime:
    (void)galay_kernel_runtime_stop(&runtime);
    (void)galay_kernel_runtime_destroy(&runtime);
cleanup_file:
    unlink(template_path);
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
