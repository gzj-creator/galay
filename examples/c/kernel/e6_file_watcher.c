#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

typedef struct ExampleState {
    atomic_int done;
    atomic_int code;
    atomic_uint events;
    atomic_int is_dir;
    char path[512];
} ExampleState;

static void on_watch(galay_kernel_file_watcher_watch_result_t* result, void* ctx)
{
    ExampleState* state = (ExampleState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_FileWatcherIOFailed : (int)result->code);
    atomic_store(&state->events, result == 0 ? 0u : (unsigned int)result->events);
    atomic_store(&state->is_dir, result == 0 ? 0 : result->is_dir);
    atomic_store(&state->done, 1);
}

static int append_to_file(const char* path)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        return 1;
    }

    const char payload[] = "demo";
    int failed = write(fd, payload, sizeof(payload) - 1) != (ssize_t)(sizeof(payload) - 1);
    close(fd);
    return failed;
}

static int wait_done(atomic_int* done)
{
    struct timespec pause = {0, 10000000};
    for (int i = 0; i < 200; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_file_watcher_t watcher = {0};
    ExampleState state;
    char template_path[] = "/tmp/galay-c-file-watch-example-XXXXXX";
    int fd = mkstemp(template_path);
    int wd = -1;
    int exit_code = 0;

    if (fd < 0) {
        return 1;
    }
    close(fd);

    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_FileWatcherIOFailed);
    atomic_init(&state.events, 0u);
    atomic_init(&state.is_dir, 0);

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_file_watcher_create(&watcher) != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_add_watch(&watcher, template_path,
            (C_FileWatchEvent)(C_FileWatchEventModify | C_FileWatchEventCloseWrite), &wd) != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_get_path(&watcher, wd, state.path, sizeof(state.path)) != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_watch(&runtime, &watcher, on_watch, &state) != C_FileWatcherSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    for (int i = 0; i < 100 && !atomic_load(&state.done); ++i) {
        if (append_to_file(template_path) != 0) {
            exit_code = 3;
            goto cleanup;
        }
        if (wait_done(&state.done) == 0) {
            break;
        }
    }

    if (!atomic_load(&state.done) || atomic_load(&state.code) != (int)C_FileWatcherSuccess) {
        exit_code = 4;
        goto cleanup;
    }

    printf("file_watcher path=%s events=0x%x is_dir=%d\n",
           state.path,
           atomic_load(&state.events),
           atomic_load(&state.is_dir));

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
