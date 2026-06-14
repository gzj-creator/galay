/**
 * @file b1_compute.cc
 * @brief 用途：压测 `ComputeScheduler` 在不同负载下的吞吐与延迟表现。
 * 关键覆盖点：空任务、轻重计算任务、不同调度器数量以及样本中位数统计。
 * 通过条件：预热与正式统计都能完成，输出性能结果且进程无崩溃、死锁或超时。
 */

#include <iostream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <thread>
#include <memory>
#include "benchmark/benchmark_sync.h"
#include "kernel/kernel/compute_scheduler.h"
#include "kernel/kernel/task.h"
#include "test/stdout_log.h"

using namespace galay::kernel;
using namespace std::chrono_literals;

// ============== 压测参数 ==============
constexpr int WARMUP_COUNT = 1000;           // 预热任务数
constexpr int THROUGHPUT_TASKS = 100000;     // 吞吐量测试任务数
constexpr int HEAVY_THROUGHPUT_TASKS = 50000;
constexpr int LATENCY_TASKS = 10000;         // 延迟测试任务数
constexpr int COMPUTE_ITERATIONS = 1000;     // 计算密集型迭代次数
constexpr std::size_t THROUGHPUT_SAMPLE_COUNT = 5;

struct BenchState {
    std::atomic<int64_t> completed{0};
    std::atomic<int64_t> latency_sum_ns{0};
    std::atomic<int64_t> latency_count{0};
    galay::benchmark::CompletionLatch* completion_latch = nullptr;
};

struct ThroughputSample {
    double elapsed_ms;
    double throughput;
};

void markCompleted(BenchState* state) {
    state->completed.fetch_add(1, std::memory_order_relaxed);
    if (state->completion_latch) {
        state->completion_latch->arrive();
    }
}

// ============== 测试协程 ==============

// 空协程（测试调度开销）
Task<void> emptyTask(BenchState* state) {
    markCompleted(state);
    co_return;
}

// 轻量计算协程
Task<void> lightComputeTask(BenchState* state) {
    volatile int sum = 0;
    for (int i = 0; i < 100; ++i) {
        sum += i;
    }
    markCompleted(state);
    co_return;
}

// 计算密集型协程
Task<void> heavyComputeTask(BenchState* state) {
    volatile double result = 0;
    for (int i = 0; i < COMPUTE_ITERATIONS; ++i) {
        result += std::sin(i) * std::cos(i);
    }
    markCompleted(state);
    co_return;
}

// 延迟测试协程
Task<void> latencyTask(BenchState* state,
                       std::chrono::steady_clock::time_point submitted_at) {
    auto now = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - submitted_at).count();
    state->latency_sum_ns.fetch_add(latency, std::memory_order_relaxed);
    state->latency_count.fetch_add(1, std::memory_order_relaxed);
    markCompleted(state);
    co_return;
}

// ============== 多调度器管理器 ==============
class SchedulerPool {
public:
    explicit SchedulerPool(int count) : m_count(count), m_next(0) {
        m_schedulers.reserve(count);
        for (int i = 0; i < count; ++i) {
            m_schedulers.push_back(std::make_unique<ComputeScheduler>());
        }
    }

    void start() {
        for (auto& s : m_schedulers) {
            s->start();
        }
    }

    void stop() {
        for (auto& s : m_schedulers) {
            s->stop();
        }
    }

    void spawn(Task<void> task) {
        // 轮询分发
        int idx = m_next.fetch_add(1, std::memory_order_relaxed) % m_count;
        scheduleTask(*m_schedulers[idx], std::move(task));
    }

    int count() const { return m_count; }

private:
    int m_count;
    std::atomic<int> m_next;
    std::vector<std::unique_ptr<ComputeScheduler>> m_schedulers;
};

ThroughputSample measureThroughputSample(
    int scheduler_count,
    int task_count,
    const std::function<Task<void>(BenchState*)>& task_factory) {
    SchedulerPool pool(scheduler_count);
    pool.start();

    galay::benchmark::CompletionLatch warmup_latch(WARMUP_COUNT);
    BenchState warmup_state;
    warmup_state.completion_latch = &warmup_latch;
    for (int i = 0; i < WARMUP_COUNT; ++i) {
        pool.spawn(task_factory(&warmup_state));
    }
    warmup_latch.wait();

    galay::benchmark::CompletionLatch measure_latch(static_cast<std::size_t>(task_count));
    BenchState measure_state;
    measure_state.completion_latch = &measure_latch;
    const auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < task_count; ++i) {
        pool.spawn(task_factory(&measure_state));
    }
    measure_latch.wait();

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    pool.stop();

    return {
        .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
        .throughput = elapsed_ns > 0
            ? (static_cast<double>(task_count) * 1'000'000'000.0 / elapsed_ns)
            : 0.0,
    };
}

// ============== 压测函数 ==============

// 吞吐量测试
void benchThroughput(const std::string& name, int scheduler_count, int task_count,
                     const std::function<Task<void>(BenchState*)>& task_factory) {
    std::vector<ThroughputSample> samples;
    samples.reserve(THROUGHPUT_SAMPLE_COUNT);
    for (std::size_t sample_index = 0; sample_index < THROUGHPUT_SAMPLE_COUNT; ++sample_index) {
        samples.push_back(measureThroughputSample(scheduler_count, task_count, task_factory));
    }
    const auto median_sample = galay::benchmark::medianElement(
        std::move(samples),
        [](const ThroughputSample& lhs, const ThroughputSample& rhs) {
            return lhs.throughput < rhs.throughput;
        });

    LogInfo("[{}] schedulers={}, tasks={}, time={}ms, throughput={:.0f} tasks/sec",
            name, scheduler_count, task_count, median_sample.elapsed_ms, median_sample.throughput);
}

// 延迟测试
void benchLatency(int scheduler_count, int task_count) {
    SchedulerPool pool(scheduler_count);
    pool.start();

    // 预热
    galay::benchmark::CompletionLatch warmup_latch(WARMUP_COUNT);
    BenchState warmup_state;
    warmup_state.completion_latch = &warmup_latch;
    for (int i = 0; i < WARMUP_COUNT; ++i) {
        pool.spawn(emptyTask(&warmup_state));
    }
    warmup_latch.wait();

    // 正式测试
    int64_t latency_sum_ns = 0;
    for (int i = 0; i < task_count; ++i) {
        galay::benchmark::CompletionLatch measure_latch(1);
        BenchState measure_state;
        measure_state.completion_latch = &measure_latch;
        pool.spawn(latencyTask(&measure_state, std::chrono::steady_clock::now()));
        measure_latch.wait();
        latency_sum_ns += measure_state.latency_sum_ns.load(std::memory_order_relaxed);
    }

    pool.stop();

    double avg_latency_us =
        static_cast<double>(latency_sum_ns) /
        static_cast<double>(task_count) / 1000.0;

    LogInfo("[Latency] schedulers={}, tasks={}, avg_latency={:.2f}us",
            scheduler_count, task_count, avg_latency_us);
}

// 扩展性测试
void benchScalability() {
    LogInfo("--- Scalability Test (heavy compute tasks) ---");

    std::vector<int> scheduler_counts = {1, 2, 4, 8};
    int task_count = 1000;

    double baseline_throughput = 0;

    for (int schedulers : scheduler_counts) {
        SchedulerPool pool(schedulers);
        pool.start();

        galay::benchmark::CompletionLatch completion_latch(static_cast<std::size_t>(task_count));
        BenchState state;
        state.completion_latch = &completion_latch;
        auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < task_count; ++i) {
            pool.spawn(heavyComputeTask(&state));
        }
        completion_latch.wait();

        auto elapsed = std::chrono::steady_clock::now() - start;
        const auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        const double elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
        const double throughput =
            elapsed_ns > 0 ? (static_cast<double>(task_count) * 1'000'000'000.0 / elapsed_ns) : 0.0;

        pool.stop();

        if (schedulers == 1) {
            baseline_throughput = throughput;
        }

        double speedup = throughput / baseline_throughput;

        LogInfo("  schedulers={}: time={}ms, throughput={:.0f}/s, speedup={:.2f}x",
                schedulers, elapsed_ms, throughput, speedup);
    }
}

// 持续压力测试
void benchSustained(int scheduler_count, int duration_sec) {
    LogInfo("--- Sustained Load Test ({}s) ---", duration_sec);

    SchedulerPool pool(scheduler_count);
    pool.start();
    BenchState state;

    auto start = std::chrono::steady_clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    std::atomic<bool> running{true};

    // 生产者线程
    std::thread producer([&]() {
        while (running) {
            pool.spawn(lightComputeTask(&state));
            // 控制提交速率
            if (state.completed.load(std::memory_order_relaxed) < 10000) {
                continue;
            }
            std::this_thread::sleep_for(1us);
        }
    });

    // 监控线程
    int64_t last_completed = 0;
    while (std::chrono::steady_clock::now() < end_time) {
        std::this_thread::sleep_for(1s);
        int64_t current = state.completed.load(std::memory_order_relaxed);
        int64_t delta = current - last_completed;
        LogInfo("  throughput: {}/s, total: {}", delta, current);
        last_completed = current;
    }

    running = false;
    producer.join();

    // 等待剩余任务完成
    std::this_thread::sleep_for(100ms);
    pool.stop();

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    double avg_throughput =
        static_cast<double>(state.completed.load(std::memory_order_relaxed)) / ms * 1000.0;

    LogInfo("  total: {} tasks in {}ms, avg throughput: {:.0f}/s",
            state.completed.load(std::memory_order_relaxed), ms, avg_throughput);
}

int main(int argc, char* argv[]) {
    const unsigned hardware_threads = std::max(1u, std::thread::hardware_concurrency());
    int scheduler_count = galay::benchmark::defaultBenchmarkSchedulerCount(hardware_threads);
    if (argc > 1) {
        scheduler_count = std::max(1, std::atoi(argv[1]));
    }

    LogInfo("=== ComputeScheduler Benchmark ===");
    LogInfo("CPU cores: {}, using {} schedulers", std::thread::hardware_concurrency(), scheduler_count);
    LogInfo("");

    // 1. 吞吐量测试 - 空任务
    LogInfo("--- Throughput Test (empty tasks) ---");
    benchThroughput("Empty", scheduler_count, THROUGHPUT_TASKS, emptyTask);

    LogInfo("");

    // 2. 吞吐量测试 - 轻量计算
    LogInfo("--- Throughput Test (light compute) ---");
    benchThroughput("Light", scheduler_count, THROUGHPUT_TASKS, lightComputeTask);

    LogInfo("");

    // 3. 吞吐量测试 - 重计算
    LogInfo("--- Throughput Test (heavy compute) ---");
    benchThroughput("Heavy", scheduler_count, HEAVY_THROUGHPUT_TASKS, heavyComputeTask);

    LogInfo("");

    // 4. 延迟测试
    LogInfo("--- Latency Test ---");
    // 延迟更适合测轻度并发下的调度开销，避免大 burst 排队时间淹没 wakeup/schedule 成本。
    benchLatency(std::min(scheduler_count, 2), LATENCY_TASKS);

    LogInfo("");

    // 5. 扩展性测试
    benchScalability();

    LogInfo("");

    // 6. 持续压力测试
    benchSustained(scheduler_count, 5);

    LogInfo("");
    LogInfo("=== Benchmark Complete ===");

    return 0;
}
