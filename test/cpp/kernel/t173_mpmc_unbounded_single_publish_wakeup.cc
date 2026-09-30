/**
 * @file t173_mpmc_unbounded_single_publish_wakeup.cc
 * @brief 压测单次发布与接收 waiter arming 并发时不丢唤醒。
 */

namespace {
void recvPumpPublishedTestPoint() noexcept;
}

#define GALAY_MPMC_UNBOUNDED_PUMP_PUBLISHED_TEST_POINT() \
    recvPumpPublishedTestPoint()
#include <galay/cpp/galay-kernel/concurrency/mpmc/unbounded_channel.h>
#undef GALAY_MPMC_UNBOUNDED_PUMP_PUBLISHED_TEST_POINT
#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "benchmark/cpp/common/benchmark_affinity.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

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
    static void requestPump(UnboundedChannel<uint64_t>& channel) noexcept
    {
        channel.requestRecvPump();
    }

    static void publishWorkForCleanup(UnboundedChannel<uint64_t>& channel) noexcept
    {
        // Rescue only a failing test's stale CAS loop so its thread can join.
        const auto previous = channel.m_recvPumpState.fetch_or(
            channel.kRecvWork, std::memory_order_release);
        (void)previous; // The test needs publication, not the previous state.
    }

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

struct PumpPublishGate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

thread_local PumpPublishGate* pumpPublishGate = nullptr;

void recvPumpPublishedTestPoint() noexcept
{
    auto* gate = pumpPublishGate;
    if (gate == nullptr) {
        return;
    }
    pumpPublishGate = nullptr;
    gate->entered.store(true, std::memory_order_release);
    while (!gate->release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

#if defined(__SANITIZE_THREAD__)
constexpr uint64_t kIterations = 50;
#else
// 每次迭代都是一次完整的 publish→wakeup→consume 握手，规模只需覆盖唤醒竞态；
// 更大的规模在高负载并行 ctest 下会被调度延迟放大而超时。
constexpr uint64_t kIterations = 1'000;
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


// A successful await_suspend ticket identifies a specific receive registration;
// approximate waiter-queue length is not a completion/parking handshake.
struct ParkedState {
    std::atomic<uint64_t> armed{0};
    std::atomic<uint64_t> received{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> done{false};
};

template <typename Awaitable>
struct ObservedReceive {
    Awaitable inner;
    ParkedState* state;
    uint64_t ticket;

    bool await_ready() noexcept { return inner.await_ready(); }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        auto* observed = state;
        const uint64_t next = ticket;
        const bool suspended = inner.await_suspend(handle);
        if (!suspended) {
            observed->failed.store(true, std::memory_order_release);
            return false;
        }
        // Sender may immediately wake/destroy the awaiter after publication.
        observed->armed.store(next, std::memory_order_release);
        return true;
    }

    auto await_resume() noexcept { return inner.await_resume(); }
};

template <size_t Batch>
Task<void> receiveParked(galay::mpmc::UnboundedChannel<uint64_t>* channel,
                         ParkedState* state, uint64_t rounds) {
    for (uint64_t i = 0; i < rounds; ++i) {
        bool valid = true;
        if constexpr (Batch == 1) {
            auto value = co_await ObservedReceive{channel->recv(), state, i + 1};
            valid = value && *value == i;
        } else {
            auto values = co_await ObservedReceive{channel->recvBatch(Batch), state, i + 1};
            valid = values && values->size() == Batch;
            if (valid) {
                for (size_t j = 0; j < Batch; ++j) {
                    valid = valid && (*values)[j] == i * Batch + j;
                }
            }
        }
        if (!valid) {
            state->failed.store(true, std::memory_order_release);
            state->done.store(true, std::memory_order_release);
            co_return;
        }
        state->received.store(i + 1, std::memory_order_release);
    }
    auto closed = co_await ObservedReceive{channel->recv(), state, rounds + 1};
    if (closed || !galay::kernel::IOError::contains(closed.error().code(), galay::kernel::kClosed)) {
        state->failed.store(true, std::memory_order_release);
    }
    state->done.store(true, std::memory_order_release);
}

template <size_t Batch, bool UseToken>
bool runParkedLifecycle() {
    constexpr uint64_t kRounds = 256;
    galay::mpmc::UnboundedChannel<uint64_t> channel;
    ParallelScheduler scheduler;
    ParkedState state;
    const auto started = scheduler.start();
    if (!started) { return false; }
    bool ok = scheduleTask(scheduler, receiveParked<Batch>(&channel, &state, kRounds));
    auto token = channel.makeProducerToken();
    ok = ok && token.valid();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    auto waitArmed = [&](uint64_t ticket) {
        while (state.armed.load(std::memory_order_acquire) != ticket &&
               !state.done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return state.armed.load(std::memory_order_acquire) == ticket;
    };
    for (uint64_t i = 0; ok && i < kRounds; ++i) {
        ok = waitArmed(i + 1);
        if (!ok) { break; }
        if constexpr (Batch == 1) {
            uint64_t value = i;
            ok = UseToken ? channel.send(token, std::move(value)) : channel.send(std::move(value));
        } else {
            std::vector<uint64_t> values(Batch);
            for (size_t j = 0; j < Batch; ++j) { values[j] = i * Batch + j; }
            ok = channel.sendBatch(token, std::move(values));
        }
    }
    ok = ok && waitArmed(kRounds + 1);
    const auto snapshot = galay::mpmc::UnboundedChannelTestAccess::snapshot(channel);
    ok = ok && snapshot.waiterPath != 0 && state.received.load(std::memory_order_acquire) == kRounds;
    channel.close();
    while (!state.done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ok = ok && state.done.load(std::memory_order_acquire) &&
        !state.failed.load(std::memory_order_acquire);
    scheduler.stop();
    ok = ok && channel.empty() && !channel.send(uint64_t{0});
    if (!ok) { std::cerr << "T173 parked lifecycle failed batch=" << Batch << " token=" << UseToken << '\n'; }
    return ok;
}

bool runPumpOwnerHandoff()
{
    using Access = galay::mpmc::UnboundedChannelTestAccess;
    galay::mpmc::UnboundedChannel<uint64_t> channel;
    ParallelScheduler scheduler;
    ParkedState state;
    const auto started = scheduler.start();
    if (!started) {
        return false;
    }
    bool ok = scheduleTask(scheduler, receiveParked<1>(&channel, &state, 1));
    auto waitFor = [](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return ready();
    };
    ok = ok && waitFor([&] {
        return state.armed.load(std::memory_order_acquire) == 1;
    });
    if (!ok) {
        channel.close();
        scheduler.stop();
        return false;
    }

    PumpPublishGate gate;
    std::atomic<bool> senderDone{false};
    std::atomic<bool> sent{false};
    std::thread sender([&] {
        auto token = channel.makeProducerToken();
        if (token.valid()) {
            pumpPublishGate = &gate;
            sent.store(channel.send(token, uint64_t{0}), std::memory_order_release);
            pumpPublishGate = nullptr;
        }
        senderDone.store(true, std::memory_order_release);
    });
    ok = waitFor([&] { return gate.entered.load(std::memory_order_acquire); });
    if (ok) {
        // Sender published work but has not tried to claim the pump. Another
        // requester drains it and returns the state to idle before sender's CAS.
        Access::requestPump(channel);
        ok = waitFor([&] {
            return state.armed.load(std::memory_order_acquire) == 2;
        });
        ok = ok && state.received.load(std::memory_order_acquire) == 1 &&
            Access::snapshot(channel).pumpState == 0;
    }
    gate.release.store(true, std::memory_order_release);
    const bool returned = waitFor([&] {
        return senderDone.load(std::memory_order_acquire);
    });
    if (!returned) {
        std::cerr << "T173 pump handoff stalled after competing owner returned idle\n";
        Access::publishWorkForCleanup(channel);
    }
    sender.join();
    channel.close();
    const bool receiverDone = waitFor([&] {
        return state.done.load(std::memory_order_acquire);
    });
    scheduler.stop();
    return ok && returned && sent.load(std::memory_order_acquire) &&
        receiverDone && !state.failed.load(std::memory_order_acquire) &&
        channel.empty() && Access::snapshot(channel).pumpState == 0;
}

} // namespace

int main()
{
    if (!runPumpOwnerHandoff()) {
        return 1;
    }
    if (!runParkedLifecycle<1, true>() || !runParkedLifecycle<1, false>() ||
        !runParkedLifecycle<8, true>()) { return 1; }
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

    // 内部 deadline 必须小于 ctest TIMEOUT(45s)，保证自身先给出诊断输出
    const auto deadline = std::chrono::steady_clock::now() + 30s;
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
