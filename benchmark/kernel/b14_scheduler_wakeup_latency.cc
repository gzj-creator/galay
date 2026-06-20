/**
 * @file b14_wakeup.cc
 * @brief 用途：压测跨线程注入任务后的 scheduler 唤醒吞吐与延迟。
 * 关键覆盖点：多生产者远端注入、完成计数、延迟采样与唤醒收敛。
 * 通过条件：压测样本全部完成并输出结果，进程无崩溃、死锁或超时。
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "benchmark/benchmark_sync.h"
#include <galay/cpp/galay-kernel/common/timer_manager.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/stdout_log.h"

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using IOSchedulerType = galay::kernel::KqueueScheduler;
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using IOSchedulerType = galay::kernel::EpollScheduler;
#elif defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

constexpr int kProducerCount = 4;
constexpr int kTasksPerProducer = 50000;
constexpr int kLatencySamples = 10000;
constexpr int kLatencyWarmupSamples = 2000;
constexpr int kSkewedTaskCount = 4000;
constexpr auto kSkewedTaskWork = 200us;

struct BenchState {
    std::atomic<int64_t> completed{0};
    std::atomic<int64_t> latency_sum_ns{0};
    galay::benchmark::CompletionLatch* completion_latch = nullptr;
};

bool waitUntil(auto&& predicate,
               std::chrono::milliseconds timeout = 2000ms,
               std::chrono::milliseconds step = 1ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return predicate();
}

void burnFor(std::chrono::microseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

Task<void> throughputTask(BenchState* state) {
    state->completed.fetch_add(1, std::memory_order_relaxed);
    if (state->completion_latch) {
        state->completion_latch->arrive();
    }
    co_return;
}

Task<void> latencyTask(BenchState* state,
                       std::chrono::steady_clock::time_point submitted_at) {
    const auto now = std::chrono::steady_clock::now();
    const auto latency_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - submitted_at).count();
    state->latency_sum_ns.fetch_add(latency_ns, std::memory_order_relaxed);
    state->completed.fetch_add(1, std::memory_order_relaxed);
    if (state->completion_latch) {
        state->completion_latch->arrive();
    }
    co_return;
}

struct SkewBenchState {
    std::atomic<int64_t> completed{0};
    std::atomic<int64_t> ran_on_source{0};
    std::atomic<int64_t> ran_on_sibling{0};
    galay::benchmark::CompletionLatch* completion_latch = nullptr;
    IOScheduler* source = nullptr;
    IOScheduler* sibling = nullptr;
};

struct SkewBenchResult {
    double elapsed_ms = 0.0;
    double throughput = 0.0;
    int64_t ran_on_source = 0;
    int64_t ran_on_sibling = 0;
    uint64_t steal_attempts = 0;
    uint64_t steal_successes = 0;
};

Task<void> skewedRuntimeTask(SkewBenchState* state) {
    const auto tid = std::this_thread::get_id();
    if (state->source && tid == state->source->threadId()) {
        state->ran_on_source.fetch_add(1, std::memory_order_relaxed);
    } else if (state->sibling && tid == state->sibling->threadId()) {
        state->ran_on_sibling.fetch_add(1, std::memory_order_relaxed);
    }

    burnFor(kSkewedTaskWork);
    state->completed.fetch_add(1, std::memory_order_relaxed);
    if (state->completion_latch) {
        state->completion_latch->arrive();
    }
    co_return;
}

template <typename SchedulerT>
void runThroughputBenchmark() {
    SchedulerT scheduler;
    BenchState state;
    const int64_t total_tasks = static_cast<int64_t>(kProducerCount) * kTasksPerProducer;
    galay::benchmark::CompletionLatch completion_latch(static_cast<std::size_t>(total_tasks));
    state.completion_latch = &completion_latch;

    scheduler.start();
    auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);

    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&scheduler, &state]() {
            for (int i = 0; i < kTasksPerProducer; ++i) {
                scheduleTask(scheduler, throughputTask(&state));
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }
    completion_latch.wait();

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    const double throughput =
        elapsed_ns > 0 ? (static_cast<double>(total_tasks) * 1'000'000'000.0 / elapsed_ns) : 0.0;

    LogInfo("[InjectedThroughput] producers={}, tasks_per_producer={}, total={}, time={}ms, throughput={:.0f} tasks/s",
            kProducerCount,
            kTasksPerProducer,
            total_tasks,
            elapsed_ms,
            throughput);
    scheduler.stop();
}

template <typename SchedulerT>
void runLatencyBenchmark() {
    SchedulerT scheduler;
    scheduler.start();

    for (int i = 0; i < kLatencyWarmupSamples; ++i) {
        BenchState warmup_state;
        galay::benchmark::CompletionLatch warmup_latch(1);
        warmup_state.completion_latch = &warmup_latch;
        scheduleTask(scheduler, throughputTask(&warmup_state));
        warmup_latch.wait();
    }

    auto start = std::chrono::steady_clock::now();
    int64_t latency_sum_ns = 0;
    for (int i = 0; i < kLatencySamples; ++i) {
        BenchState state;
        galay::benchmark::CompletionLatch completion_latch(1);
        state.completion_latch = &completion_latch;
        scheduleTask(scheduler, latencyTask(&state, std::chrono::steady_clock::now()));
        completion_latch.wait();
        latency_sum_ns += state.latency_sum_ns.load(std::memory_order_relaxed);
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    const double avg_latency_us =
        static_cast<double>(latency_sum_ns) /
        static_cast<double>(kLatencySamples) / 1000.0;

    LogInfo("[InjectedLatency] samples={}, time={}ms, avg_latency={:.2f}us",
            kLatencySamples,
            elapsed_ms,
            avg_latency_us);
    scheduler.stop();
}

template <typename SchedulerT>
SkewBenchResult runSingleSchedulerSkewBaseline() {
    SchedulerT scheduler;
    SkewBenchState state;
    galay::benchmark::CompletionLatch completion_latch(static_cast<std::size_t>(kSkewedTaskCount));
    state.completion_latch = &completion_latch;

    scheduler.replaceTimerManager(TimingWheelTimerManager(1'000'000ULL));
    scheduler.start();
    const bool ready = waitUntil([&]() {
        return scheduler.threadId() != std::thread::id{};
    });
    if (!ready) {
        throw std::runtime_error("single scheduler benchmark thread did not start");
    }

    state.source = &scheduler;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kSkewedTaskCount; ++i) {
        scheduleTask(scheduler, skewedRuntimeTask(&state));
    }
    completion_latch.wait();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    scheduler.stop();

    return {
        .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
        .throughput = elapsed_ns > 0
            ? (static_cast<double>(kSkewedTaskCount) * 1'000'000'000.0 / elapsed_ns)
            : 0.0,
        .ran_on_source = state.ran_on_source.load(std::memory_order_relaxed),
        .ran_on_sibling = state.ran_on_sibling.load(std::memory_order_relaxed),
        .steal_attempts = 0,
        .steal_successes = 0,
    };
}

SkewBenchResult runTwoSchedulerStealBenchmark() {
    Runtime runtime;
    auto source = std::make_unique<IOSchedulerType>();
    auto sibling = std::make_unique<IOSchedulerType>();
    source->replaceTimerManager(TimingWheelTimerManager(1'000'000ULL));
    sibling->replaceTimerManager(TimingWheelTimerManager(1'000'000ULL));
    auto* source_ptr = source.get();
    auto* sibling_ptr = sibling.get();
    runtime.addIOScheduler(std::move(source));
    runtime.addIOScheduler(std::move(sibling));
    runtime.start();

    const bool ready = waitUntil([&]() {
        return source_ptr->threadId() != std::thread::id{} &&
               sibling_ptr->threadId() != std::thread::id{};
    });
    if (!ready) {
        runtime.stop();
        throw std::runtime_error("runtime skew benchmark scheduler threads did not start");
    }

    SkewBenchState state;
    galay::benchmark::CompletionLatch completion_latch(static_cast<std::size_t>(kSkewedTaskCount));
    state.completion_latch = &completion_latch;
    state.source = source_ptr;
    state.sibling = sibling_ptr;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kSkewedTaskCount; ++i) {
        scheduleTask(*source_ptr, skewedRuntimeTask(&state));
    }
    completion_latch.wait();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    runtime.stop();
    const auto stats = runtime.stats();

    uint64_t steal_attempts = 0;
    uint64_t steal_successes = 0;
    for (const auto& io : stats.io_schedulers) {
        steal_attempts += io.steal_attempts;
        steal_successes += io.steal_successes;
    }

    return {
        .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
        .throughput = elapsed_ns > 0
            ? (static_cast<double>(kSkewedTaskCount) * 1'000'000'000.0 / elapsed_ns)
            : 0.0,
        .ran_on_source = state.ran_on_source.load(std::memory_order_relaxed),
        .ran_on_sibling = state.ran_on_sibling.load(std::memory_order_relaxed),
        .steal_attempts = steal_attempts,
        .steal_successes = steal_successes,
    };
}

}  // namespace

int main() {
#if defined(USE_KQUEUE)
    KqueueScheduler scheduler;
    constexpr const char* backend = "kqueue";
#elif defined(USE_EPOLL)
    EpollScheduler scheduler;
    constexpr const char* backend = "epoll";
#elif defined(USE_IOURING)
    IOUringScheduler scheduler;
    constexpr const char* backend = "io_uring";
#else
    std::cout << "B14-SchedulerInjectedWakeup SKIP\n";
    return 0;
#endif

    LogInfo("Scheduler injected wakeup benchmark, backend={}", backend);
    (void)scheduler;

    runThroughputBenchmark<std::decay_t<decltype(scheduler)>>();
    std::this_thread::sleep_for(50ms);
    runLatencyBenchmark<std::decay_t<decltype(scheduler)>>();

    const auto baseline = runSingleSchedulerSkewBaseline<std::decay_t<decltype(scheduler)>>();
    const auto skewed = runTwoSchedulerStealBenchmark();
    const double sibling_share = kSkewedTaskCount > 0
        ? (100.0 * static_cast<double>(skewed.ran_on_sibling) /
           static_cast<double>(kSkewedTaskCount))
        : 0.0;
    const double speedup = baseline.throughput > 0.0
        ? (skewed.throughput / baseline.throughput)
        : 0.0;
    const double hit_rate = skewed.steal_attempts > 0
        ? (100.0 * static_cast<double>(skewed.steal_successes) /
           static_cast<double>(skewed.steal_attempts))
        : 0.0;

    LogInfo("[SkewedSingleScheduler] total={}, work={}us, time={}ms, throughput={:.0f} tasks/s",
            kSkewedTaskCount,
            kSkewedTaskWork.count(),
            baseline.elapsed_ms,
            baseline.throughput);
    LogInfo("[SkewedTwoSchedulerSteal] total={}, work={}us, time={}ms, throughput={:.0f} tasks/s, source={}, sibling={}, sibling_share={:.1f}%",
            kSkewedTaskCount,
            kSkewedTaskWork.count(),
            skewed.elapsed_ms,
            skewed.throughput,
            skewed.ran_on_source,
            skewed.ran_on_sibling,
            sibling_share);
    LogInfo("[SkewedStealStats] attempts={}, successes={}, hit_rate={:.1f}%",
            skewed.steal_attempts,
            skewed.steal_successes,
            hit_rate);
    LogInfo("[SkewedStealSpeedup] speedup={:.2f}x", speedup);
    return 0;
}
