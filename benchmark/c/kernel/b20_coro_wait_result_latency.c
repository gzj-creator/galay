#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_wait_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <time.h>

enum {
    kIterations = 20000,
};

typedef struct WaitBenchState {
    C_CoroWaitRequest request;
    atomic_int phase;
    atomic_int errors;
    atomic_llong resume_ns;
    uint64_t generation;
    int iterations;
} WaitBenchState;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int compare_i64(const void* lhs, const void* rhs)
{
    int64_t a = *(const int64_t*)lhs;
    int64_t b = *(const int64_t*)rhs;
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

static void wait_bench_entry(void* arg)
{
    WaitBenchState* state = (WaitBenchState*)arg;
    for (int i = 0; i < state->iterations; ++i) {
        uint64_t generation = 0;
        if (galay_coro_wait_request_prepare(&state->request, &generation).code != C_IOResultOk) {
            atomic_fetch_add_explicit(&state->errors, 1, memory_order_release);
            return;
        }
        state->generation = generation;
        atomic_store_explicit(&state->phase, i * 2 + 1, memory_order_release);

        C_IOResult result = galay_coro_wait(&state->request, -1);
        atomic_store_explicit(&state->resume_ns, now_ns(), memory_order_release);
        if (result.code != C_IOResultOk || result.value != i) {
            atomic_fetch_add_explicit(&state->errors, 1, memory_order_release);
            return;
        }
        atomic_store_explicit(&state->phase, i * 2 + 2, memory_order_release);
    }
}

static int wait_for_phase(WaitBenchState* state, int phase)
{
    const int64_t deadline = now_ns() + 5000000000LL;
    int spins = 0;
    while (now_ns() < deadline) {
        if (atomic_load_explicit(&state->phase, memory_order_acquire) >= phase) {
            return 0;
        }
        ++spins;
        if ((spins & 0xff) == 0) {
            sched_yield();
        }
    }
    return 1;
}

int main(void)
{
    int64_t* latencies = (int64_t*)calloc((size_t)kIterations, sizeof(int64_t));
    if (latencies == 0) {
        return 1;
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    WaitBenchState state = {
        .request = {0},
        .phase = ATOMIC_VAR_INIT(0),
        .errors = ATOMIC_VAR_INIT(0),
        .resume_ns = ATOMIC_VAR_INIT(0),
        .generation = 0,
        .iterations = kIterations,
    };

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_coro_wait_request_create(&state.request).code != C_IOResultOk) {
        free(latencies);
        return 2;
    }

    galay_coro_task_t task = {0};
    if (galay_coro_spawn(&runtime, wait_bench_entry, &state, 0, &task).code != C_IOResultOk) {
        free(latencies);
        return 3;
    }

    int64_t elapsed_start = now_ns();
    for (int i = 0; i < kIterations; ++i) {
        if (wait_for_phase(&state, i * 2 + 1)) {
            fprintf(stderr,
                    "wait ready phase timed out: iteration=%d phase=%d errors=%d\n",
                    i,
                    atomic_load_explicit(&state.phase, memory_order_acquire),
                    atomic_load_explicit(&state.errors, memory_order_acquire));
            free(latencies);
            return 4;
        }
        C_IOResult result = {
            .code = C_IOResultOk,
            .sys_errno = 0,
            .bytes = 0,
            .value = i,
            .ptr = 0,
        };
        int64_t start = now_ns();
        atomic_store_explicit(&state.resume_ns, 0, memory_order_release);
        if (galay_coro_wait_request_complete(&state.request, state.generation, result).code !=
            C_IOResultOk) {
            fprintf(stderr,
                    "complete failed: iteration=%d generation=%llu phase=%d errors=%d\n",
                    i,
                    (unsigned long long)state.generation,
                    atomic_load_explicit(&state.phase, memory_order_acquire),
                    atomic_load_explicit(&state.errors, memory_order_acquire));
            free(latencies);
            return 5;
        }
        if (wait_for_phase(&state, i * 2 + 2)) {
            fprintf(stderr,
                    "wait resume phase timed out: iteration=%d generation=%llu phase=%d errors=%d resume_ns=%lld\n",
                    i,
                    (unsigned long long)state.generation,
                    atomic_load_explicit(&state.phase, memory_order_acquire),
                    atomic_load_explicit(&state.errors, memory_order_acquire),
                    (long long)atomic_load_explicit(&state.resume_ns, memory_order_acquire));
            free(latencies);
            return 6;
        }
        int64_t resume_ns = atomic_load_explicit(&state.resume_ns, memory_order_acquire);
        latencies[i] = resume_ns > start ? resume_ns - start : 0;
    }
    int64_t elapsed_end = now_ns();

    if (galay_coro_join(&task, 1000).code != C_IOResultOk ||
        galay_coro_destroy(&task).code != C_IOResultOk ||
        galay_coro_wait_request_destroy(&state.request).code != C_IOResultOk) {
        free(latencies);
        return 7;
    }
    (void)galay_kernel_runtime_stop(&runtime);
    (void)galay_kernel_runtime_destroy(&runtime);

    int errors = atomic_load_explicit(&state.errors, memory_order_acquire);
    int64_t sum = 0;
    for (int i = 0; i < kIterations; ++i) {
        sum += latencies[i];
    }
    qsort(latencies, (size_t)kIterations, sizeof(int64_t), compare_i64);

    int64_t elapsed_ns = elapsed_end - elapsed_start;
    double qps = elapsed_ns > 0
        ? (double)kIterations * 1000000000.0 / (double)elapsed_ns
        : 0.0;
    printf("CoroWaitResultLatency mode=single_outstanding, samples=%d, qps=%.0f, avg=%.2fus, p50=%.2fus, p90=%.2fus, p99=%.2fus, errors=%d\n",
           kIterations,
           qps,
           (double)sum / (double)kIterations / 1000.0,
           percentile_us(latencies, kIterations, 0.50),
           percentile_us(latencies, kIterations, 0.90),
           percentile_us(latencies, kIterations, 0.99),
           errors);

    free(latencies);
    return errors == 0 ? 0 : 8;
}
