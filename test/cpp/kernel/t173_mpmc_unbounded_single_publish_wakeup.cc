/**
 * @file t173_mpmc_unbounded_single_publish_wakeup.cc
 * @brief 压测单次发布与接收 waiter arming 并发时不丢唤醒。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "benchmark/cpp/common/benchmark_affinity.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

struct WakeupSnapshot {
    uint64_t head = 0;
    uint64_t tail = 0;
    uint64_t headSequence = 0;
    size_t waiterCount = 0;
    uint8_t pumpState = 0;
    uint8_t waiterPath = 0;
};

namespace galay::mpmc {

struct UnboundedChannelTestAccess
{
    static WakeupSnapshot snapshot(UnboundedChannel<uint64_t>& channel) noexcept
    {
        WakeupSnapshot result;
        result.head = channel.m_head.load(std::memory_order_acquire);
        result.tail = channel.m_tail.load(std::memory_order_acquire);
        result.waiterCount = channel.m_recvWaiters.size_approx();
        result.pumpState =
            channel.m_recvPumpState.load(std::memory_order_acquire);
        result.waiterPath =
            channel.m_recvWaiterPathUsed.load(std::memory_order_acquire);

        auto* block = channel.m_headBlock.load(std::memory_order_acquire);
        const uint64_t targetBase =
            result.head & ~(channel.kSlotsPerBlock - 1);
        while (block != nullptr &&
               block->base.load(std::memory_order_acquire) < targetBase) {
            block = block->next.load(std::memory_order_acquire);
        }
        if (block != nullptr &&
            block->base.load(std::memory_order_acquire) == targetBase) {
            result.headSequence = block->slots[static_cast<size_t>(
                result.head & (channel.kSlotsPerBlock - 1))]
                                      .sequence.load(std::memory_order_acquire);
        }
        return result;
    }
};

} // namespace galay::mpmc

namespace {

using galay::kernel::ParallelScheduler;
using galay::kernel::Task;
using namespace std::chrono_literals;

#if defined(__SANITIZE_THREAD__)
constexpr uint64_t kIterations = 50;
#else
constexpr uint64_t kIterations = 5'000;
#endif

void cpuPause() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

struct StressState {
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> consumed{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> done{false};
    std::atomic<bool> stop{false};
    std::atomic<galay::benchmark::ThreadPlacement> producerPlacement{
        galay::benchmark::ThreadPlacement::kUnsupported};
    std::atomic<galay::benchmark::ThreadPlacement> consumerPlacement{
        galay::benchmark::ThreadPlacement::kUnsupported};
};

Task<void> receiveOneAtATime(
    galay::mpmc::UnboundedChannel<uint64_t>* channel,
    StressState* state)
{
    state->consumerPlacement.store(
        galay::benchmark::ThreadPlacement::kUnsupported,
        std::memory_order_release);
    for (uint64_t expected = 1; expected <= kIterations; ++expected) {
        auto value = co_await channel->recv();
        if (!value || *value != expected) {
            state->failed.store(true, std::memory_order_release);
            break;
        }
        state->consumed.store(expected, std::memory_order_release);
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    galay::mpmc::UnboundedChannel<uint64_t> channel;
    StressState state;
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        std::cerr << "T173 scheduler start failed\n";
        return 1;
    }
    if (!scheduleTask(scheduler, receiveOneAtATime(&channel, &state))) {
        scheduler.stop();
        std::cerr << "T173 receiver schedule failed\n";
        return 1;
    }

    std::thread producer([&]() {
        state.producerPlacement.store(
            galay::benchmark::ThreadPlacement::kUnsupported,
            std::memory_order_release);
        auto token = channel.makeProducerToken();
        if (!token.valid()) {
            state.failed.store(true, std::memory_order_release);
            state.stop.store(true, std::memory_order_release);
            return;
        }
        for (uint64_t value = 1; value <= kIterations; ++value) {
            while (state.consumed.load(std::memory_order_acquire) != value - 1) {
                if (state.stop.load(std::memory_order_acquire)) {
                    return;
                }
                cpuPause();
                std::this_thread::yield();
            }
            uint64_t pending = value;
            if (!channel.send(token, std::move(pending))) {
                state.failed.store(true, std::memory_order_release);
                state.stop.store(true, std::memory_order_release);
                return;
            }
            state.sent.store(value, std::memory_order_release);
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (!state.done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool completed = state.done.load(std::memory_order_acquire);
    const WakeupSnapshot snapshot =
        galay::mpmc::UnboundedChannelTestAccess::snapshot(channel);
    if (!completed) {
        state.failed.store(true, std::memory_order_release);
    }
    state.stop.store(true, std::memory_order_release);
    channel.close();
    producer.join();
    scheduler.stop();

    const bool passed = completed &&
        !state.failed.load(std::memory_order_acquire) &&
        state.consumed.load(std::memory_order_acquire) == kIterations;
    if (!passed) {
        std::cerr << "T173 failed consumed="
                  << state.consumed.load(std::memory_order_acquire)
                  << " sent=" << state.sent.load(std::memory_order_acquire)
                  << " completed=" << completed
                  << " head=" << snapshot.head
                  << " tail=" << snapshot.tail
                  << " head_sequence=" << snapshot.headSequence
                  << " waiters=" << snapshot.waiterCount
                  << " pump_state=" << static_cast<unsigned>(snapshot.pumpState)
                  << " waiter_path=" << static_cast<unsigned>(snapshot.waiterPath)
                  << '\n';
        return 1;
    }
    std::cout << "T173-MpmcUnboundedSinglePublishWakeup PASS iterations="
              << kIterations << " producer_placement="
              << galay::benchmark::threadPlacementName(
                     state.producerPlacement.load(std::memory_order_acquire))
              << " consumer_placement="
              << galay::benchmark::threadPlacementName(
                     state.consumerPlacement.load(std::memory_order_acquire))
              << '\n';
    return 0;
}
