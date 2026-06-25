#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct TimeoutState {
    atomic_int done;
    atomic_int code;
} TimeoutState;

static void on_watch_timeout(galay_kernel_file_watcher_watch_result_t* result, void* ctx)
{
    TimeoutState* state = (TimeoutState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_FileWatcherIOFailed : (int)result->code);
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

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

int main(int argc, char** argv)
{
    int iterations = 64;
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
    char template_path[] = "/tmp/galay-c-file-watch-timeout-bench-XXXXXX";
    int fd = mkstemp(template_path);
    int wd = -1;
    int completed = 0;
    int failures = 0;

    if (fd < 0) {
        return 1;
    }
    close(fd);

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        failures = 1;
        goto cleanup;
    }

    C_FileWatcherResultCode create_code = galay_kernel_file_watcher_create(&watcher);
    if (create_code == C_FileWatcherOperationUnsupported) {
        printf("file_watcher_timeout_smoke unsupported backend; skipped\n");
        goto cleanup;
    }
    if (create_code != C_FileWatcherSuccess ||
        galay_kernel_file_watcher_add_watch(&watcher, template_path, C_FileWatchEventModify, &wd) !=
            C_FileWatcherSuccess) {
        failures = 1;
        goto cleanup;
    }

    const int64_t start = now_us();
    for (int i = 0; i < iterations; ++i) {
        TimeoutState state;
        atomic_init(&state.done, 0);
        atomic_init(&state.code, (int)C_FileWatcherSuccess);

        if (galay_kernel_file_watcher_watch_timeout(&runtime, &watcher, 1, on_watch_timeout, &state) !=
            C_FileWatcherSuccess) {
            ++failures;
            break;
        }
        if (wait_done(&state.done) != 0 || atomic_load(&state.code) != (int)C_FileWatcherTimeout) {
            ++failures;
            break;
        }
        ++completed;
    }
    const int64_t elapsed = now_us() - start;

    printf("file_watcher_timeout_smoke iterations=%d completed=%d failures=%d elapsed_us=%lld avg_us=%.2f\n",
           iterations,
           completed,
           failures,
           (long long)elapsed,
           completed == 0 ? 0.0 : (double)elapsed / (double)completed);

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
    return failures == 0 ? 0 : 1;
}
