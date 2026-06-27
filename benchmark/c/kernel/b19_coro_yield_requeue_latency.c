#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum {
    kIterationsPerTask = 10000,
    kTaskCount = 2
};

static const char* backend_name(void)
{
#if defined(USE_KQUEUE)
    return "kqueue";
#elif defined(USE_EPOLL)
    return "epoll";
#elif defined(USE_IOURING)
    return "io_uring";
#else
    return "unknown";
#endif
}

typedef struct BenchState {
    int iterations;
    int64_t* latencies_ns;
    int* cursor;
    int* errors;
} BenchState;

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

static void bench_entry(void* arg)
{
    BenchState* state = (BenchState*)arg;
    for (int i = 0; i < state->iterations; ++i) {
        int64_t start = now_ns();
        C_IOResult yielded = galay_coro_yield();
        int64_t end = now_ns();
        if (yielded.code != C_IOResultOk) {
            ++(*state->errors);
        }
        state->latencies_ns[(*state->cursor)++] = end - start;
    }
}

int main(void)
{
    const int total_samples = kIterationsPerTask * kTaskCount;
    int64_t* latencies = (int64_t*)calloc((size_t)total_samples, sizeof(int64_t));
    if (latencies == NULL) {
        return 1;
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        free(latencies);
        return 2;
    }

    int cursor = 0;
    int errors = 0;
    BenchState state = {
        .iterations = kIterationsPerTask,
        .latencies_ns = latencies,
        .cursor = &cursor,
        .errors = &errors,
    };
    C_CoroOptions options = galay_coro_options_default();
    galay_coro_task_t tasks[kTaskCount] = {{0}, {0}};

    for (int i = 0; i < kTaskCount; ++i) {
        if (galay_coro_spawn(&runtime, bench_entry, &state, &options, &tasks[i]).code != C_IOResultOk) {
            free(latencies);
            (void)galay_kernel_runtime_destroy(&runtime);
            return 3;
        }
    }

    int64_t elapsed_start = now_ns();
    for (int i = 0; i < kTaskCount; ++i) {
        if (galay_coro_join(&tasks[i], 5000).code != C_IOResultOk) {
            free(latencies);
            (void)galay_kernel_runtime_destroy(&runtime);
            return 4;
        }
    }
    int64_t elapsed_end = now_ns();

    for (int i = 0; i < kTaskCount; ++i) {
        (void)galay_coro_destroy(&tasks[i]);
    }
    (void)galay_kernel_runtime_stop(&runtime);
    (void)galay_kernel_runtime_destroy(&runtime);

    if (cursor != total_samples) {
        free(latencies);
        return 5;
    }

    int64_t sum = 0;
    for (int i = 0; i < cursor; ++i) {
        sum += latencies[i];
    }
    qsort(latencies, (size_t)cursor, sizeof(int64_t), compare_i64);

    int64_t elapsed_ns = elapsed_end - elapsed_start;
    double qps = elapsed_ns > 0
        ? (double)cursor * 1000000000.0 / (double)elapsed_ns
        : 0.0;
    printf("CoroSchedulerYieldLatency mode=owner_yield_requeue, samples=%d, qps=%.0f, avg=%.2fus, p50=%.2fus, p90=%.2fus, p99=%.2fus, errors=%d\n",
           cursor,
           qps,
           (double)sum / (double)cursor / 1000.0,
           percentile_us(latencies, cursor, 0.50),
           percentile_us(latencies, cursor, 0.90),
           percentile_us(latencies, cursor, 0.99),
           errors);
    printf("CoroSchedulerYieldLatencyDetails backend=%s, io_schedulers=1, errors=%d, avg_ns=%.0f, p50_ns=%.0f, p90_ns=%.0f, p99_ns=%.0f\n",
           backend_name(),
           errors,
           (double)sum / (double)cursor,
           percentile_us(latencies, cursor, 0.50) * 1000.0,
           percentile_us(latencies, cursor, 0.90) * 1000.0,
           percentile_us(latencies, cursor, 0.99) * 1000.0);

    free(latencies);
    return 0;
}
