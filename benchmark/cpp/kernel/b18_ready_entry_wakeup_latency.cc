/**
 * @file b18_ready_entry_wakeup_latency.cc
 * @brief 采集 ReadyEntry ready queue 下跨线程唤醒 C++ Task 的延迟分布。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/waker.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

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

constexpr int kWarmupSamples = 512;
constexpr int kMeasuredSamples = 5000;

int64_t nowNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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

struct SampleState {
    Waker waker;
    std::atomic<bool> armed{false};
    std::atomic<bool> done{false};
    std::atomic<int64_t> submitted_ns{0};
    std::atomic<int64_t> latency_ns{0};
};

class ReusableWakeProducer {
public:
    ReusableWakeProducer()
        : producer_([this]() { run(); })
    {
    }

    ~ReusableWakeProducer() {
        stop();
    }

    bool submit(SampleState* state) {
        if (state == nullptr) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || pending_ != nullptr) {
                return false;
            }
            pending_ = state;
        }
        cv_.notify_one();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
        }
        cv_.notify_one();
        if (producer_.joinable()) {
            producer_.join();
        }
    }

private:
    void run() {
        while (true) {
            SampleState* state = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() {
                    return stopping_ || pending_ != nullptr;
                });
                if (stopping_ && pending_ == nullptr) {
                    return;
                }
                state = pending_;
                pending_ = nullptr;
            }

            state->submitted_ns.store(nowNanoseconds(), std::memory_order_release);
            state->waker.wakeUp();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    SampleState* pending_ = nullptr;
    bool stopping_ = false;
    std::thread producer_;
};

struct ManualSuspendAwaitable {
    SampleState* state;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        state->waker = Waker(handle);
        state->armed.store(true, std::memory_order_release);
        return true;
    }

    void await_resume() const noexcept {}
};

Task<void> measuredWakeTask(SampleState* state) {
    co_await ManualSuspendAwaitable{state};
    const int64_t submitted = state->submitted_ns.load(std::memory_order_acquire);
    state->latency_ns.store(nowNanoseconds() - submitted, std::memory_order_release);
    state->done.store(true, std::memory_order_release);
    co_return;
}

double percentileMicros(const std::vector<int64_t>& sorted_ns, double percentile) {
    if (sorted_ns.empty()) {
        return 0.0;
    }
    const auto index = static_cast<size_t>(
        (static_cast<double>(sorted_ns.size() - 1) * percentile) / 100.0);
    return static_cast<double>(sorted_ns[index]) / 1000.0;
}

bool runSamples(IOScheduler* scheduler,
                ReusableWakeProducer& producer,
                int samples,
                std::vector<int64_t>* latencies) {
    for (int i = 0; i < samples; ++i) {
        SampleState state;
        if (!scheduleTask(*scheduler, measuredWakeTask(&state))) {
            std::cerr << "[B18] failed to schedule sample task\n";
            return false;
        }
        if (!waitUntil([&]() { return state.armed.load(std::memory_order_acquire); })) {
            std::cerr << "[B18] sample task did not arm waker\n";
            return false;
        }

        if (!producer.submit(&state)) {
            std::cerr << "[B18] failed to submit sample wake to producer thread\n";
            return false;
        }

        if (!waitUntil([&]() { return state.done.load(std::memory_order_acquire); })) {
            std::cerr << "[B18] sample task did not resume\n";
            return false;
        }
        if (latencies != nullptr) {
            latencies->push_back(state.latency_ns.load(std::memory_order_acquire));
        }
    }
    return true;
}

}  // namespace

int main() {
#if defined(USE_KQUEUE)
    constexpr const char* backend = "kqueue";
#elif defined(USE_EPOLL)
    constexpr const char* backend = "epoll";
#elif defined(USE_IOURING)
    constexpr const char* backend = "io_uring";
#else
    std::cout << "B18-ReadyEntryWakeupLatency SKIP\n";
    return 0;
#endif

    Runtime runtime;
    auto scheduler = std::make_unique<IOSchedulerType>();
    auto* scheduler_ptr = scheduler.get();
    if (!runtime.addIOScheduler(std::move(scheduler))) {
        std::cerr << "[B18] failed to add IO scheduler\n";
        return 1;
    }

    auto started = runtime.start();
    if (!started.has_value()) {
        std::cerr << "[B18] failed to start runtime\n";
        return 1;
    }

    ReusableWakeProducer producer;
    if (!runSamples(scheduler_ptr, producer, kWarmupSamples, nullptr)) {
        runtime.stop();
        return 1;
    }

    std::vector<int64_t> latencies;
    latencies.reserve(kMeasuredSamples);
    const auto bench_start = std::chrono::steady_clock::now();
    if (!runSamples(scheduler_ptr, producer, kMeasuredSamples, &latencies)) {
        runtime.stop();
        return 1;
    }
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - bench_start).count();
    runtime.stop();

    std::sort(latencies.begin(), latencies.end());
    int64_t sum = 0;
    for (const int64_t value : latencies) {
        sum += value;
    }
    const double avg_us = latencies.empty()
        ? 0.0
        : static_cast<double>(sum) / static_cast<double>(latencies.size()) / 1000.0;
    producer.stop();

    const double sampled_wakes_per_sec = elapsed_ns > 0
        ? static_cast<double>(latencies.size()) * 1'000'000'000.0 / static_cast<double>(elapsed_ns)
        : 0.0;

    std::cout << "ReadyEntry wakeup latency benchmark, backend=" << backend << "\n";
    std::cout << std::fixed << std::setprecision(2)
              << "[ReadyEntryWakeupLatency] samples=" << latencies.size()
              << ", avg=" << avg_us << "us"
              << ", p50=" << percentileMicros(latencies, 50.0) << "us"
              << ", p90=" << percentileMicros(latencies, 90.0) << "us"
              << ", p99=" << percentileMicros(latencies, 99.0) << "us"
              << ", sampled_wakes_per_sec=" << std::setprecision(0)
              << sampled_wakes_per_sec << "\n";
    return 0;
}
