#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <errno.h>
#include <fcntl.h>
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
    galay_kernel_file_watcher_t* watcher;
    galay_kernel_file_watcher_watch_result_t watch_result;
    C_IOResult result;
} WatchState;

static void watch_entry(void* arg)
{
    WatchState* state = (WatchState*)arg;
    state->result = galay_kernel_file_watcher_watch(state->watcher, &state->watch_result, 2000);
}

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static int append_to_file(const char* path)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        return 1;
    }

    const char payload[] = "x";
    int failed = write(fd, payload, sizeof(payload) - 1) != (ssize_t)(sizeof(payload) - 1);
    if (close(fd) != 0) {
        failed = 1;
    }
    return failed;
}

static int wait_task_or_trigger(const char* path, galay_coro_task_t* task)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        C_IOResult poll = galay_coro_join(task, 0);
        if (poll.code == C_IOResultOk) {
            return 0;
        }
        if (poll.code != C_IOResultTimeout) {
            return 1;
        }
        if ((i % 10) == 0 && append_to_file(path) != 0) {
            return 1;
        }
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR) {
            return 1;
        }
    }
    return 1;
}

static int parse_iterations(int argc, char** argv)
{
    int iterations = FILE_WATCHER_DEFAULT_ITERATIONS;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0) {
            iterations = parsed;
        }
    }
    return iterations;
}

static int unlink_if_exists(const char* path)
{
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (unlink(path) == 0) {
        return 0;
    }
    return errno == ENOENT ? 0 : 1;
}

int main(int argc, char** argv)
{
    const int iterations = parse_iterations(argc, argv);

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
    if (close(fd) != 0) {
        return 2;
    }
    fd = -1;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_file_watcher_create(&watcher) != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_add_watch(&watcher, template_path,
            (C_FileWatchEvent)(C_FileWatchEventModify | C_FileWatchEventCloseWrite), &wd) != C_FileWatcherSuccess) {
        exit_code = 3;
        goto cleanup;
    }

    const int64_t start = now_us();
    for (int i = 0; i < iterations; ++i) {
        WatchState state = {
            .watcher = &watcher,
            .watch_result = {0},
            .result = {C_IOResultInvalid, 0, 0, 0, NULL},
        };
        galay_coro_task_t task = {0};
        C_IOResult spawn_result = galay_coro_spawn(&runtime, watch_entry, &state, NULL, &task);
        if (spawn_result.code != C_IOResultOk ||
            wait_task_or_trigger(template_path, &task) != 0 ||
            state.result.code != C_IOResultOk ||
            state.watch_result.code != C_FileWatcherSuccess) {
            if (task.task != NULL &&
                galay_coro_destroy(&task).code != C_IOResultOk &&
                exit_code == 0) {
                exit_code = 4;
            }
            if (exit_code == 0) {
                exit_code = 5;
            }
            goto cleanup_with_elapsed;
        }
        C_IOResult destroy_result = galay_coro_destroy(&task);
        if (destroy_result.code != C_IOResultOk) {
            exit_code = 6;
            goto cleanup_with_elapsed;
        }
        ++events_seen;
    }

cleanup_with_elapsed:
    {
        const int64_t elapsed = now_us() - start;
        const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
        const double events_per_sec = seconds > 0.0 ? (double)events_seen / seconds : 0.0;
        if (printf("file_watcher_events iterations=%d events=%d elapsed_ms=%.3f events_per_sec=%.2f\n",
                   iterations,
                   events_seen,
                   (double)elapsed / 1000.0,
                   events_per_sec) < 0 &&
            exit_code == 0) {
            exit_code = 7;
        }
    }

cleanup:
    if (watcher.watcher != NULL) {
        if (wd >= 0 &&
            galay_kernel_file_watcher_remove_watch(&watcher, wd) != C_FileWatcherSuccess &&
            exit_code == 0) {
            exit_code = 8;
        }
        if (galay_kernel_file_watcher_destroy(&watcher) != C_FileWatcherSuccess && exit_code == 0) {
            exit_code = 9;
        }
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 10;
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 11;
    }
    if (fd >= 0 && close(fd) != 0 && exit_code == 0) {
        exit_code = 12;
    }
    if (unlink_if_exists(template_path) != 0 && exit_code == 0) {
        exit_code = 13;
    }
    return exit_code;
}
