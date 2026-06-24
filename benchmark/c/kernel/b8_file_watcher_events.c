#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>

#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum {
    FILE_WATCHER_DEFAULT_ITERATIONS = 64
};

typedef struct WatchState {
    atomic_int done;
    atomic_int code;
    atomic_uint events;
} WatchState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void on_watch(galay_kernel_file_watcher_watch_result_t* result, void* ctx)
{
    WatchState* state = (WatchState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_FileWatcherIOFailed : (int)result->code);
    atomic_store(&state->events, result == 0 ? 0u : (unsigned int)result->events);
    atomic_store(&state->done, 1);
}

static int append_to_file(const char* path)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        return 1;
    }

    const char payload[] = "x";
    int failed = write(fd, payload, sizeof(payload) - 1) != (ssize_t)(sizeof(payload) - 1);
    close(fd);
    return failed;
}

static int wait_done_or_trigger(const char* path, atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        if ((i % 10) == 0 && append_to_file(path) != 0) {
            return 1;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

int main(int argc, char** argv)
{
    int iterations = FILE_WATCHER_DEFAULT_ITERATIONS;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0) {
            iterations = parsed;
        }
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_file_watcher_t watcher = {0};
    char template_path[] = "/tmp/galay-c-file-watch-bench-XXXXXX";
    int fd = mkstemp(template_path);
    int wd = -1;
    int events_seen = 0;
    int exit_code = 0;

    if (fd < 0) {
        return 1;
    }
    close(fd);

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_file_watcher_create(&watcher) != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_add_watch(&watcher, template_path,
            (C_FileWatchEvent)(C_FileWatchEventModify | C_FileWatchEventCloseWrite), &wd) != C_FileWatcherSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    const int64_t start = now_us();
    for (int i = 0; i < iterations; ++i) {
        WatchState state;
        atomic_init(&state.done, 0);
        atomic_init(&state.code, (int)C_FileWatcherIOFailed);
        atomic_init(&state.events, 0u);

        if (galay_kernel_file_watcher_watch(&runtime, &watcher, on_watch, &state) != C_FileWatcherSuccess ||
            wait_done_or_trigger(template_path, &state.done) != 0 ||
            atomic_load(&state.code) != (int)C_FileWatcherSuccess) {
            exit_code = 3;
            goto cleanup_with_elapsed;
        }
        ++events_seen;
    }

cleanup_with_elapsed:
    {
        const int64_t elapsed = now_us() - start;
        const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
        const double events_per_sec = seconds > 0.0 ? (double)events_seen / seconds : 0.0;
        printf("file_watcher_events iterations=%d events=%d elapsed_ms=%.3f events_per_sec=%.2f\n",
               iterations,
               events_seen,
               (double)elapsed / 1000.0,
               events_per_sec);
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
