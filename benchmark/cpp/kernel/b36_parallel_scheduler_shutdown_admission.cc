/**
 * @file b36_parallel_scheduler_shutdown_admission.cc
 * @brief 测量停机与生产者发生竞态时 ParallelScheduler 的工作接纳情况。
 */

#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>

using galay::kernel::ParallelScheduler;
using galay::kernel::ParallelWorkItem;

namespace {

struct Counters {
    std::atomic<std::size_t> executed{0};
};

void runWork(void* context, std::size_t) noexcept
{
    static_cast<Counters*>(context)->executed.fetch_add(
        1, std::memory_order_relaxed);
}

void releaseWork(void*) noexcept {}

struct Blocker {
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
};

void runBlocker(void* context, std::size_t) noexcept
{
    auto* blocker = static_cast<Blocker*>(context);
    blocker->started.store(true, std::memory_order_release);
    while (!blocker->release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void releaseBlocker(void*) noexcept {}

bool runRound(std::size_t producer_count,
              std::size_t attempts_per_producer,
              std::size_t& accepted,
              std::size_t& executed)
{
    ParallelScheduler scheduler;
    if (!scheduler.start().has_value()) {
        return false;
    }

    Blocker blocker;
    if (!scheduler.scheduleWork(
            ParallelWorkItem(&blocker, 0, &runBlocker, &releaseBlocker))) {
        scheduler.stop();
        return false;
    }
    while (!blocker.started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    Counters counters;
    std::atomic<bool> producing{true};
    std::atomic<std::size_t> round_accepted{0};
    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&]() {
            for (std::size_t attempt = 0;
                 attempt < attempts_per_producer &&
                 producing.load(std::memory_order_acquire);
                 ++attempt) {
                if (scheduler.scheduleWork(
                        ParallelWorkItem(&counters, attempt, &runWork, &releaseWork))) {
                    round_accepted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::thread stopper([&]() { scheduler.stop(); });
    while (scheduler.isRunning()) {
        std::this_thread::yield();
    }
    producing.store(false, std::memory_order_release);
    blocker.release.store(true, std::memory_order_release);
    stopper.join();
    for (auto& producer : producers) {
        producer.join();
    }

    accepted += round_accepted.load(std::memory_order_acquire);
    executed += counters.executed.load(std::memory_order_acquire);
    return round_accepted.load(std::memory_order_acquire) ==
        counters.executed.load(std::memory_order_acquire);
}

}  // namespace

int main()
{
    constexpr std::size_t kRounds = 100;
    constexpr std::size_t kProducerCount = 8;
    constexpr std::size_t kAttemptsPerProducer = 256;

    std::size_t accepted = 0;
    std::size_t executed = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t round = 0; round < kRounds; ++round) {
        if (!runRound(kProducerCount,
                      kAttemptsPerProducer,
                      accepted,
                      executed)) {
            std::cerr << "parallel_shutdown_admission status=failed\n";
            return 1;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin);
    const auto total = kRounds * kProducerCount * kAttemptsPerProducer;
    const double seconds = static_cast<double>(elapsed.count()) / 1'000'000.0;
    std::cout << "parallel_shutdown_admission rounds=" << kRounds
              << " attempts=" << total
              << " accepted=" << accepted
              << " executed=" << executed
              << " elapsed_us=" << elapsed.count()
              << " accepted_per_sec="
              << (seconds > 0.0 ? static_cast<double>(accepted) / seconds : 0.0)
              << "\n";
    return accepted == executed ? 0 : 1;
}
