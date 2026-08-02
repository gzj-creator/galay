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
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include "benchmark/cpp/common/benchmark_sync.h"
#include <galay/cpp/galay-kernel/common/timer_manager.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/cpp/common/stdout_log.h"

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
constexpr int kScalingTaskCount = 4000;
constexpr auto kScalingTaskWork = 200us;

struct BenchState {
    std::atomic<int64_t> completed{0};
    std::atomic<int64_t> latency_sum_ns{0};
    galay::benchmark::CompletionLatch* completion_latch = nullptr;
};

void addCounter(std::atomic<int64_t>& counter, int64_t value = 1) noexcept {
    const int64_t previous = counter.fetch_add(value, std::memory_order_relaxed);
    if (value > 0 && previous > std::numeric_limits<int64_t>::max() - value) {
        counter.store(std::numeric_limits<int64_t>::max(), std::memory_order_relaxed);
    }
}

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
    addCounter(state->completed);
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
    addCounter(state->latency_sum_ns, latency_ns);
    addCounter(state->completed);
    if (state->completion_latch) {
        state->completion_latch->arrive();
    }
    co_return;
}

struct ScalingBenchState {
    std::atomic<int64_t> completed{0};
    std::atomic<int64_t> ran_on_first{0};
    std::atomic<int64_t> ran_on_second{0};
    galay::benchmark::CompletionLatch* completion_latch = nullptr;
    IOScheduler* first = nullptr;
    IOScheduler* second = nullptr;
};

struct ScalingBenchResult {
    double elapsed_ms = 0.0;
    double throughput = 0.0;
    int64_t ran_on_first = 0;
    int64_t ran_on_second = 0;
    bool valid = false;
};

Task<void> scalingRuntimeTask(ScalingBenchState* state) {
    const auto tid = std::this_thread::get_id();
    if (state->first && tid == state->first->threadId()) {
        addCounter(state->ran_on_first);
    } else if (state->second && tid == state->second->threadId()) {
        addCounter(state->ran_on_second);
    }

    burnFor(kScalingTaskWork);
    addCounter(state->completed);
    if (state->completion_latch) {
        state->completion_latch->arrive();
    }
    co_return;
}

template <typename SchedulerT>
bool runThroughputBenchmark() {
    SchedulerT scheduler;
    BenchState state;
    const int64_t total_tasks = static_cast<int64_t>(kProducerCount) * kTasksPerProducer;
    galay::benchmark::CompletionLatch completion_latch(static_cast<std::size_t>(total_tasks));
    state.completion_latch = &completion_latch;

    const auto started = scheduler.start();
    if (!started) {
        LogError("Injected throughput scheduler failed to start: {}", started.error().message());
        return false;
    }
    auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    std::atomic<int64_t> submit_failures{0};

    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&scheduler, &state, &completion_latch, &submit_failures]() {
            for (int i = 0; i < kTasksPerProducer; ++i) {
                if (!scheduleTask(scheduler, throughputTask(&state))) {
                    addCounter(submit_failures);
                    completion_latch.arrive();
                }
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
    if (submit_failures.load(std::memory_order_relaxed) != 0) {
        LogError("Injected throughput submit failures={}",
                 submit_failures.load(std::memory_order_relaxed));
        return false;
    }
    return true;
}

template <typename SchedulerT>
bool runLatencyBenchmark() {
    SchedulerT scheduler;
    const auto started = scheduler.start();
    if (!started) {
        LogError("Injected latency scheduler failed to start: {}", started.error().message());
        return false;
    }

    for (int i = 0; i < kLatencyWarmupSamples; ++i) {
        BenchState warmup_state;
        galay::benchmark::CompletionLatch warmup_latch(1);
        warmup_state.completion_latch = &warmup_latch;
        if (!scheduleTask(scheduler, throughputTask(&warmup_state))) {
            scheduler.stop();
            LogError("Injected latency warmup submit failed at sample={}", i);
            return false;
        }
        warmup_latch.wait();
    }

    auto start = std::chrono::steady_clock::now();
    int64_t latency_sum_ns = 0;
    for (int i = 0; i < kLatencySamples; ++i) {
        BenchState state;
        galay::benchmark::CompletionLatch completion_latch(1);
        state.completion_latch = &completion_latch;
        if (!scheduleTask(scheduler, latencyTask(&state, std::chrono::steady_clock::now()))) {
            scheduler.stop();
            LogError("Injected latency submit failed at sample={}", i);
            return false;
        }
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
    return true;
}

template <typename SchedulerT>
ScalingBenchResult runSingleSchedulerBaseline() {
    SchedulerT scheduler;
    ScalingBenchState state;
    galay::benchmark::CompletionLatch completion_latch(static_cast<std::size_t>(kScalingTaskCount));
    state.completion_latch = &completion_latch;

    scheduler.replaceTimerManager(TimingWheelTimerManager(1'000'000ULL));
    const auto started = scheduler.start();
    if (!started) {
        LogError("Single scheduler baseline failed to start: {}", started.error().message());
        return {};
    }
    const bool ready = waitUntil([&]() {
        return scheduler.threadId() != std::thread::id{};
    });
    if (!ready) {
        scheduler.stop();
        LogError("Single scheduler benchmark thread did not start");
        return {};
    }

    state.first = &scheduler;
    const auto start = std::chrono::steady_clock::now();
    bool submitted = true;
    for (int i = 0; i < kScalingTaskCount; ++i) {
        if (!scheduleTask(scheduler, scalingRuntimeTask(&state))) {
            completion_latch.arrive(static_cast<std::size_t>(kScalingTaskCount - i));
            submitted = false;
            break;
        }
    }
    completion_latch.wait();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    scheduler.stop();

    return {
        .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
        .throughput = elapsed_ns > 0
            ? (static_cast<double>(kScalingTaskCount) * 1'000'000'000.0 / elapsed_ns)
            : 0.0,
        .ran_on_first = state.ran_on_first.load(std::memory_order_relaxed),
        .ran_on_second = state.ran_on_second.load(std::memory_order_relaxed),
        .valid = submitted &&
            state.completed.load(std::memory_order_relaxed) == kScalingTaskCount,
    };
}

ScalingBenchResult runTwoSchedulerRoundRobinBenchmark() {
    Runtime runtime;
    auto first = std::make_unique<IOSchedulerType>();
    auto second = std::make_unique<IOSchedulerType>();
    first->replaceTimerManager(TimingWheelTimerManager(1'000'000ULL));
    second->replaceTimerManager(TimingWheelTimerManager(1'000'000ULL));
    auto* first_ptr = first.get();
    auto* second_ptr = second.get();
    runtime.addIOScheduler(std::move(first));
    runtime.addIOScheduler(std::move(second));
    const auto started = runtime.start();
    if (!started) {
        LogError("Two scheduler runtime failed to start: {}", started.error().message());
        return {};
    }

    const bool ready = waitUntil([&]() {
        return first_ptr->threadId() != std::thread::id{} &&
               second_ptr->threadId() != std::thread::id{};
    });
    if (!ready) {
        runtime.stop();
        LogError("Two scheduler runtime threads did not start");
        return {};
    }

    ScalingBenchState state;
    galay::benchmark::CompletionLatch completion_latch(static_cast<std::size_t>(kScalingTaskCount));
    state.completion_latch = &completion_latch;
    state.first = first_ptr;
    state.second = second_ptr;

    const auto start = std::chrono::steady_clock::now();
    bool submitted = true;
    for (int i = 0; i < kScalingTaskCount; ++i) {
        IOScheduler* const scheduler = runtime.getNextIOScheduler();
        if (scheduler == nullptr || !scheduleTask(*scheduler, scalingRuntimeTask(&state))) {
            completion_latch.arrive(static_cast<std::size_t>(kScalingTaskCount - i));
            submitted = false;
            break;
        }
    }
    completion_latch.wait();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    runtime.stop();

    return {
        .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
        .throughput = elapsed_ns > 0
            ? (static_cast<double>(kScalingTaskCount) * 1'000'000'000.0 / elapsed_ns)
            : 0.0,
        .ran_on_first = state.ran_on_first.load(std::memory_order_relaxed),
        .ran_on_second = state.ran_on_second.load(std::memory_order_relaxed),
        .valid = submitted &&
            state.completed.load(std::memory_order_relaxed) == kScalingTaskCount,
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

    if (!runThroughputBenchmark<std::decay_t<decltype(scheduler)>>()) {
        return 1;
    }
    std::this_thread::sleep_for(50ms);
    if (!runLatencyBenchmark<std::decay_t<decltype(scheduler)>>()) {
        return 1;
    }

    LogInfo("[IOSchedulerAffinity] stealing=disabled, load_balance=runtime-round-robin, reason=reactor-owner-affinity");
    const auto baseline = runSingleSchedulerBaseline<std::decay_t<decltype(scheduler)>>();
    const auto balanced = runTwoSchedulerRoundRobinBenchmark();
    if (!baseline.valid || !balanced.valid) {
        return 1;
    }
    const double second_share = kScalingTaskCount > 0
        ? (100.0 * static_cast<double>(balanced.ran_on_second) /
           static_cast<double>(kScalingTaskCount))
        : 0.0;
    const double speedup = baseline.throughput > 0.0
        ? (balanced.throughput / baseline.throughput)
        : 0.0;

    LogInfo("[RoundRobinSingleScheduler] total={}, work={}us, time={}ms, throughput={:.0f} tasks/s",
            kScalingTaskCount,
            kScalingTaskWork.count(),
            baseline.elapsed_ms,
            baseline.throughput);
    LogInfo("[RoundRobinTwoScheduler] total={}, work={}us, time={}ms, throughput={:.0f} tasks/s, first={}, second={}, second_share={:.1f}%",
            kScalingTaskCount,
            kScalingTaskWork.count(),
            balanced.elapsed_ms,
            balanced.throughput,
            balanced.ran_on_first,
            balanced.ran_on_second,
            second_share);
    LogInfo("[RoundRobinSchedulerSpeedup] speedup={:.2f}x", speedup);
    return 0;
}
