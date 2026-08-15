#include <galay/c/galay-kernel-c/core-c/io_controller.h>
#include <galay/c/galay-kernel-c/core-c/io_scheduler.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>
#include <galay/c/galay-kernel-c/coro-c/coro_wait.h>

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

enum {
    kIterations = 20000,
};

typedef struct WaitBenchState {
    galay_c_io_controller_t controller;
    galay_c_io_scheduler_t* scheduler;
    atomic_int phase;
    atomic_int errors;
    atomic_llong resume_ns;
    int read_fd;
    int iterations;
} WaitBenchState;

static int64_t now_ns(void)
{
    struct timespec ts;
    return clock_gettime(CLOCK_MONOTONIC, &ts) == 0
        ? (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec
        : 0;
}

static int compare_i64(const void* lhs, const void* rhs)
{
    const int64_t a = *(const int64_t*)lhs;
    const int64_t b = *(const int64_t*)rhs;
    return (a > b) - (a < b);
}

static double percentile_us(const int64_t* sorted, int count, double percentile)
{
    if (count <= 0) {
        return 0.0;
    }
    int index = (int)((double)(count - 1) * percentile);
    if (index < 0) {
        index = 0;
    }
    if (index >= count) {
        index = count - 1;
    }
    return (double)sorted[index] / 1000.0;
}

static int set_nonblocking_cloexec(int fd)
{
    const int status_flags = fcntl(fd, F_GETFL, 0);
    if (status_flags < 0 || fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0) {
        return 1;
    }
    const int descriptor_flags = fcntl(fd, F_GETFD, 0);
    return descriptor_flags < 0 || fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0;
}

static int write_signal(int fd)
{
    const char signal = 1;
    ssize_t written;
    do {
        written = write(fd, &signal, sizeof(signal));
    } while (written < 0 && errno == EINTR);
    return written == (ssize_t)sizeof(signal) ? 0 : 1;
}

static int read_signal(int fd)
{
    char signal = 0;
    ssize_t received;
    do {
        received = read(fd, &signal, sizeof(signal));
    } while (received < 0 && errno == EINTR);
    return received == (ssize_t)sizeof(signal) ? 0 : 1;
}

static void wait_bench_entry(void* arg)
{
    WaitBenchState* state = (WaitBenchState*)arg;
    state->scheduler = galay_c_io_scheduler_current();
    if (state->scheduler == NULL) {
        atomic_fetch_add_explicit(&state->errors, 1, memory_order_release);
        return;
    }
    for (int i = 0; i < state->iterations; ++i) {
        atomic_store_explicit(&state->phase, i * 2 + 1, memory_order_release);
        const C_IOResult result = galay_c_coro_wait_io(state->scheduler,
                                                       &state->controller,
                                                       GALAY_C_EVENT_READ,
                                                       -1);
        atomic_store_explicit(&state->resume_ns, now_ns(), memory_order_release);
        if (result.code != C_IOResultOk || read_signal(state->read_fd) != 0) {
            atomic_fetch_add_explicit(&state->errors, 1, memory_order_release);
            return;
        }
        atomic_store_explicit(&state->phase, i * 2 + 2, memory_order_release);
    }
}

static int wait_for_phase(WaitBenchState* state, int phase)
{
    const int64_t start = now_ns();
    if (start == 0) {
        return 1;
    }
    const int64_t deadline = start + 5000000000LL;
    int spins = 0;
    while (now_ns() < deadline) {
        if (atomic_load_explicit(&state->phase, memory_order_acquire) >= phase) {
            return 0;
        }
        ++spins;
        if ((spins & 0xff) == 0 && sched_yield() != 0) {
            return 1;
        }
    }
    return 1;
}

int main(void)
{
    int exit_code = 0;
    int pipe_fds[2] = {-1, -1};
    int task_joined = 0;
    int controller_initialized = 0;
    int64_t* latencies = calloc((size_t)kIterations, sizeof(int64_t));
    C_RuntimeConfig config = galay_c_runtime_config_default();
    galay_c_runtime_t runtime = {0};
    galay_c_coro_task_t task = {0};
    WaitBenchState state = {
        .scheduler = NULL,
        .phase = ATOMIC_VAR_INIT(0),
        .errors = ATOMIC_VAR_INIT(0),
        .resume_ns = ATOMIC_VAR_INIT(0),
        .read_fd = -1,
        .iterations = kIterations,
    };

    if (latencies == NULL) {
        return 1;
    }
    if (pipe(pipe_fds) != 0 || set_nonblocking_cloexec(pipe_fds[0]) != 0 ||
        set_nonblocking_cloexec(pipe_fds[1]) != 0) {
        exit_code = 2;
        goto cleanup;
    }
    state.read_fd = pipe_fds[0];
    if (galay_c_io_controller_init(&state.controller, pipe_fds[0], &state).code !=
        C_IOResultOk) {
        exit_code = 3;
        goto cleanup;
    }
    controller_initialized = 1;

    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    if (galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_c_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 4;
        goto cleanup;
    }
    if (galay_c_coro_spawn(&runtime, wait_bench_entry, &state, NULL, &task).code !=
        C_IOResultOk) {
        exit_code = 5;
        goto cleanup;
    }

    const int64_t elapsed_start = now_ns();
    if (elapsed_start == 0) {
        exit_code = 6;
        goto cleanup;
    }
    for (int i = 0; i < kIterations; ++i) {
        if (wait_for_phase(&state, i * 2 + 1) != 0) {
            exit_code = 7;
            goto cleanup;
        }
        const int64_t start = now_ns();
        atomic_store_explicit(&state.resume_ns, 0, memory_order_release);
        if (start == 0 || write_signal(pipe_fds[1]) != 0) {
            exit_code = 8;
            goto cleanup;
        }
        if (wait_for_phase(&state, i * 2 + 2) != 0) {
            exit_code = 9;
            goto cleanup;
        }
        const int64_t resume_ns =
            atomic_load_explicit(&state.resume_ns, memory_order_acquire);
        latencies[i] = resume_ns > start ? resume_ns - start : 0;
    }
    const int64_t elapsed_end = now_ns();
    if (elapsed_end == 0 || galay_c_coro_join(&task, 1000).code != C_IOResultOk) {
        exit_code = 10;
        goto cleanup;
    }
    task_joined = 1;

    const int errors = atomic_load_explicit(&state.errors, memory_order_acquire);
    int64_t sum = 0;
    for (int i = 0; i < kIterations; ++i) {
        sum += latencies[i];
    }
    qsort(latencies, (size_t)kIterations, sizeof(int64_t), compare_i64);
    const int64_t elapsed_ns = elapsed_end - elapsed_start;
    const double qps = elapsed_ns > 0
        ? (double)kIterations * 1000000000.0 / (double)elapsed_ns
        : 0.0;
    if (printf("CoroWaitResultLatency mode=native_io_wait, samples=%d, qps=%.0f, avg=%.2fus, p50=%.2fus, p90=%.2fus, p99=%.2fus, errors=%d\n",
               kIterations,
               qps,
               (double)sum / (double)kIterations / 1000.0,
               percentile_us(latencies, kIterations, 0.50),
               percentile_us(latencies, kIterations, 0.90),
               percentile_us(latencies, kIterations, 0.99),
               errors) < 0 || errors != 0) {
        exit_code = 11;
    }

cleanup:
    if (task.task != NULL && !task_joined) {
        if (write_signal(pipe_fds[1]) != 0 && exit_code == 0) {
            exit_code = 12;
        }
        if (galay_c_coro_join(&task, 1000).code != C_IOResultOk && exit_code == 0) {
            exit_code = 13;
        }
    }
    if (task.task != NULL && galay_c_coro_destroy(&task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 14;
    }
    if (state.scheduler != NULL &&
        atomic_load_explicit(&state.controller.registered_events, memory_order_acquire) !=
            GALAY_C_EVENT_NONE &&
        galay_c_io_scheduler_unregister(state.scheduler, &state.controller).code !=
            C_IOResultOk &&
        exit_code == 0) {
        exit_code = 15;
    }
    if (controller_initialized &&
        galay_c_io_controller_cleanup(&state.controller).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 16;
    }
    if (runtime.runtime != NULL) {
        if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 17;
        }
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 18;
        }
    }
    if (pipe_fds[0] >= 0 && close(pipe_fds[0]) != 0 && exit_code == 0) {
        exit_code = 19;
    }
    if (pipe_fds[1] >= 0 && close(pipe_fds[1]) != 0 && exit_code == 0) {
        exit_code = 20;
    }
    free(latencies);
    return exit_code;
}
