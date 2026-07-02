/**
 * @file b20_thread_safe_timer_manager.cc
 * @brief 压测 ThreadSafeTimerManager 待处理队列 drain 到时间轮的热路径。
 *
 * 关键覆盖点：
 * - future timer: push 后一次 tick 将 pending 队列批量入轮，不应提前触发。
 * - expired timer: push 后一次 tick 批量触发已过期定时器并清空 manager。
 */

#include <galay/cpp/galay-kernel/common/safetimer_mgr.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kTimerCount = 100000;
constexpr uint64_t kTickNs = 1'000'000ULL;

struct Sample {
    double elapsed_ms = 0.0;
    double timers_per_sec = 0.0;
};

Sample makeSample(std::size_t count, std::chrono::steady_clock::duration elapsed)
{
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return {
        .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
        .timers_per_sec = elapsed_ns > 0
            ? static_cast<double>(count) * 1'000'000'000.0 / static_cast<double>(elapsed_ns)
            : 0.0,
    };
}

bool pushTimers(ThreadSafeTimerManager& manager,
                std::vector<Timer::ptr>& timers,
                std::chrono::milliseconds delay,
                std::atomic<std::size_t>& fired)
{
    timers.clear();
    timers.reserve(kTimerCount);
    for (std::size_t i = 0; i < kTimerCount; ++i) {
        auto timer = std::make_shared<CBTimer>(delay, [&fired]() {
            fired.fetch_add(1, std::memory_order_relaxed);
        });
        if (!manager.push(timer)) {
            std::cerr << "[B20] failed to push timer at index " << i << "\n";
            return false;
        }
        timers.push_back(std::move(timer));
    }
    return true;
}

bool benchFuturePendingDrain()
{
    ThreadSafeTimerManager manager(kTickNs);
    std::vector<Timer::ptr> timers;
    std::atomic<std::size_t> fired{0};

    if (!pushTimers(manager, timers, 1h, fired)) {
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    manager.tick();
    const auto sample = makeSample(kTimerCount, std::chrono::steady_clock::now() - start);

    const auto pending = manager.pendingSize();
    const auto wheel = manager.wheelSize();
    const auto fired_count = fired.load(std::memory_order_relaxed);
    std::cout << "ThreadSafeTimerManagerFutureDrain timers=" << kTimerCount
              << " elapsed_ms=" << std::fixed << std::setprecision(3) << sample.elapsed_ms
              << " timers_per_sec=" << std::setprecision(0) << sample.timers_per_sec
              << " pending=" << pending
              << " wheel=" << wheel
              << " fired=" << fired_count << "\n";

    if (pending != 0 || wheel != kTimerCount || fired_count != 0) {
        std::cerr << "[B20] future drain state mismatch"
                  << " pending=" << pending
                  << " wheel=" << wheel
                  << " fired=" << fired_count << "\n";
        return false;
    }
    return true;
}

bool benchExpiredPendingDrain()
{
    ThreadSafeTimerManager manager(kTickNs);
    std::vector<Timer::ptr> timers;
    std::atomic<std::size_t> fired{0};

    if (!pushTimers(manager, timers, 0ms, fired)) {
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    manager.tick();
    const auto sample = makeSample(kTimerCount, std::chrono::steady_clock::now() - start);

    const auto pending = manager.pendingSize();
    const auto wheel = manager.wheelSize();
    const auto fired_count = fired.load(std::memory_order_relaxed);
    std::cout << "ThreadSafeTimerManagerExpiredDrain timers=" << kTimerCount
              << " elapsed_ms=" << std::fixed << std::setprecision(3) << sample.elapsed_ms
              << " timers_per_sec=" << std::setprecision(0) << sample.timers_per_sec
              << " pending=" << pending
              << " wheel=" << wheel
              << " fired=" << fired_count << "\n";

    if (pending != 0 || wheel != 0 || fired_count != kTimerCount) {
        std::cerr << "[B20] expired drain state mismatch"
                  << " pending=" << pending
                  << " wheel=" << wheel
                  << " fired=" << fired_count << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    if (!benchFuturePendingDrain()) {
        return 1;
    }
    if (!benchExpiredPendingDrain()) {
        return 1;
    }

    std::cout << "B20-ThreadSafeTimerManager PASS\n";
    return 0;
}
