/**
 * @file t152_bounded_topology.cc
 * @brief 验证有界 SPSC/MPSC/MPMC channel 的独立实现与公开拓扑语义。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/mpsc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>
#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "benchmark/cpp/common/benchmark_sync.h"
#include "result_writer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using namespace std::chrono_literals;

using SpscChannel = galay::spsc::BoundedChannel<int>;
using StaticSpscChannel = galay::spsc::BoundedChannel<int, 8>;
using MpscChannel = galay::mpsc::BoundedChannel<int>;
using MpmcChannel = galay::mpmc::BoundedChannel<int>;

struct PotentiallyThrowingMoveAssign
{
    PotentiallyThrowingMoveAssign() = default;
    PotentiallyThrowingMoveAssign(const PotentiallyThrowingMoveAssign&) = delete;
    PotentiallyThrowingMoveAssign& operator=(
        const PotentiallyThrowingMoveAssign&) = delete;
    PotentiallyThrowingMoveAssign(PotentiallyThrowingMoveAssign&&) noexcept = default;
    PotentiallyThrowingMoveAssign& operator=(
        PotentiallyThrowingMoveAssign&& other) noexcept(false)
    {
        value = other.value;
        return *this;
    }

    int value = 0;
};

template <typename T>
concept HasCallerOwnedSpscBatch = requires(
    galay::spsc::BoundedChannel<T>& channel,
    std::span<T> output) {
    { channel.try_recv_batch(output) } noexcept -> std::same_as<size_t>;
    { channel.recv_batch_to(output) } noexcept;
};

static_assert(!std::is_same_v<SpscChannel, MpscChannel>);
static_assert(!std::is_same_v<SpscChannel, MpmcChannel>);
static_assert(!std::is_same_v<MpscChannel, MpmcChannel>);
static_assert(std::is_nothrow_default_constructible_v<StaticSpscChannel>);
static_assert(!std::is_constructible_v<StaticSpscChannel, size_t>);
static_assert(HasCallerOwnedSpscBatch<int>);
static_assert(!HasCallerOwnedSpscBatch<PotentiallyThrowingMoveAssign>);

using SpscBatchToAwaitable = decltype(
    std::declval<SpscChannel&>().recv_batch_to(std::declval<std::span<int>>()));
using SpscVectorBatchAwaitable = decltype(
    std::declval<SpscChannel&>().recv_batch(size_t{2}));

static_assert(!std::is_copy_constructible_v<SpscBatchToAwaitable>);
static_assert(std::is_nothrow_move_constructible_v<SpscBatchToAwaitable>);
static_assert(noexcept(std::declval<SpscBatchToAwaitable&>().await_ready()));
static_assert(noexcept(std::declval<SpscBatchToAwaitable&>().await_resume()));
static_assert(!noexcept(std::declval<SpscVectorBatchAwaitable&>().await_ready()));
static_assert(!noexcept(std::declval<SpscVectorBatchAwaitable&>().await_resume()));

struct AsyncState
{
    std::atomic<bool> entered{false};
    std::atomic<bool> done{false};
    bool success = false;
    int value = 0;
};

std::string read_all(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool check_independent_sources()
{
    const std::filesystem::path concurrencyRoot =
        std::filesystem::path(GALAY_SOURCE_ROOT) / "galay-kernel" / "concurrency";
    const std::array<std::filesystem::path, 6> headers{
        concurrencyRoot / "spsc" / "bounded_channel.h",
        concurrencyRoot / "spsc" / "unbounded_channel.h",
        concurrencyRoot / "mpsc" / "bounded_channel.h",
        concurrencyRoot / "mpsc" / "unbounded_channel.h",
        concurrencyRoot / "mpmc" / "bounded_channel.h",
        concurrencyRoot / "mpmc" / "unbounded_channel.h",
    };

    for (const auto& header : headers) {
        const std::string content = read_all(header);
        if (content.empty() || content.find("Topology") != std::string::npos ||
            content.find("concurrency/detail") != std::string::npos) {
            std::cerr << "channel source is not independent: " << header << '\n';
            return false;
        }
    }

    for (const auto& header : {headers[0], headers[1], headers[2], headers[3]}) {
        const std::string content = read_all(header);
        if (content.find("moodycamel") != std::string::npos) {
            std::cerr << "topology-specialized channel still uses MPMC queue: "
                      << header << '\n';
            return false;
        }
    }
    return true;
}

bool wait_for(const std::atomic<bool>& flag, std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::yield();
    }
    return flag.load(std::memory_order_acquire);
}

template <typename Channel>
galay::kernel::Task<void> send_one(Channel* channel, AsyncState* state, int value)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->send(std::move(value));
    state->success = result.has_value();
    state->done.store(true, std::memory_order_release);
    co_return;
}

template <typename Channel>
galay::kernel::Task<void> receive_one(Channel* channel, AsyncState* state)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->recv();
    if (result.has_value()) {
        state->success = true;
        state->value = *result;
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

template <typename Channel>
bool run_basic_close_and_drain()
{
    Channel channel(2);
    if (!channel.try_send(1) || !channel.try_send(2) || channel.try_send(3)) {
        return false;
    }
    channel.close();
    channel.close();
    if (!channel.is_closed() || channel.try_send(4)) {
        return false;
    }
    auto first = channel.try_recv();
    auto second = channel.try_recv();
    return first.has_value() && second.has_value() && *first == 1 && *second == 2 &&
        !channel.try_recv().has_value();
}

template <typename Channel>
bool run_waiter_progress()
{
    Channel sendChannel(2);
    if (!sendChannel.try_send(1) || !sendChannel.try_send(2)) {
        return false;
    }

    galay::kernel::ParallelScheduler sendScheduler;
    auto sendStarted = sendScheduler.start();
    if (!sendStarted) {
        return false;
    }
    AsyncState sendState;
    if (!galay::kernel::schedule_task(
            sendScheduler, send_one(&sendChannel, &sendState, 3)) ||
        !wait_for(sendState.entered) || sendState.done.load(std::memory_order_acquire)) {
        sendScheduler.stop();
        return false;
    }
    auto first = sendChannel.try_recv();
    const bool sendDone = wait_for(sendState.done);
    sendScheduler.stop();
    auto second = sendChannel.try_recv();
    auto third = sendChannel.try_recv();
    if (!sendDone || !sendState.success || !first.has_value() || !second.has_value() ||
        !third.has_value() || *first != 1 || *second != 2 || *third != 3) {
        return false;
    }

    Channel recvChannel(2);
    galay::kernel::ParallelScheduler recvScheduler;
    auto recvStarted = recvScheduler.start();
    if (!recvStarted) {
        return false;
    }
    AsyncState recvState;
    if (!galay::kernel::schedule_task(
            recvScheduler, receive_one(&recvChannel, &recvState)) ||
        !wait_for(recvState.entered) || recvState.done.load(std::memory_order_acquire)) {
        recvScheduler.stop();
        return false;
    }
    int value = 7;
    const bool sent = recvChannel.try_send(std::move(value));
    const bool recvDone = wait_for(recvState.done);
    recvScheduler.stop();
    return sent && recvDone && recvState.success && recvState.value == 7;
}

bool run_spsc_wraparound()
{
    constexpr size_t kMessageCount = 200'000;
    galay::spsc::BoundedChannel<uint64_t> channel(2);
    galay::benchmark::CompletionLatch ready(2);
    galay::benchmark::StartGate start;
    std::atomic<bool> failed{false};
    std::atomic<size_t> produced{0};
    std::atomic<size_t> consumed{0};
    const auto deadline = std::chrono::steady_clock::now() + 10s;

    std::thread producer([&]() {
        ready.arrive();
        start.wait();
        for (size_t sequence = 0; sequence < kMessageCount; ++sequence) {
            uint64_t value = sequence;
            while (!channel.try_send(std::move(value))) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }
            produced.store(sequence + 1, std::memory_order_release);
        }
    });

    std::thread consumer([&]() {
        ready.arrive();
        start.wait();
        uint64_t expected = 0;
        while (expected < kMessageCount) {
            auto value = channel.try_recv();
            if (!value.has_value()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
                continue;
            }
            if (*value != expected) {
                failed.store(true, std::memory_order_release);
                return;
            }
            ++expected;
            consumed.store(expected, std::memory_order_release);
        }
    });

    const bool allReady = ready.wait_for(2s);
    start.open();
    producer.join();
    consumer.join();
    return allReady && !failed.load(std::memory_order_acquire) && channel.empty() &&
        produced.load(std::memory_order_acquire) == kMessageCount &&
        consumed.load(std::memory_order_acquire) == kMessageCount;
}

bool run_spsc_caller_owned_batch()
{
    using Pointer = std::unique_ptr<int>;
    galay::spsc::BoundedChannel<Pointer> channel(8);

    if (!channel.try_send(std::make_unique<int>(1)) ||
        !channel.try_send(std::make_unique<int>(2)) ||
        !channel.try_send(std::make_unique<int>(3)) ||
        !channel.try_send(std::make_unique<int>(4))) {
        return false;
    }

    std::array<Pointer, 2> firstBatch{};
    const size_t firstCount = channel.try_recv_batch(std::span<Pointer>(firstBatch));
    if (firstCount != firstBatch.size() || !firstBatch[0] || !firstBatch[1] ||
        *firstBatch[0] != 1 || *firstBatch[1] != 2) {
        return false;
    }

    auto third = channel.try_recv();
    auto fourth = channel.try_recv();
    if (!third.has_value() || !fourth.has_value() || !*third || !*fourth ||
        **third != 3 || **fourth != 4 || channel.try_recv().has_value()) {
        return false;
    }

    if (channel.try_recv_batch(std::span<Pointer>{}) != 0 ||
        !channel.try_send(std::make_unique<int>(5)) ||
        !channel.try_send(std::make_unique<int>(6))) {
        return false;
    }
    channel.close();

    std::array<Pointer, 4> closedBatch{
        std::make_unique<int>(-1),
        std::make_unique<int>(-2),
        std::make_unique<int>(-3),
        std::make_unique<int>(-4),
    };
    const size_t closedCount =
        channel.try_recv_batch(std::span<Pointer>(closedBatch));
    if (closedCount != 2 || !closedBatch[0] || !closedBatch[1] ||
        !closedBatch[2] || !closedBatch[3] || *closedBatch[0] != 5 ||
        *closedBatch[1] != 6 || *closedBatch[2] != -3 ||
        *closedBatch[3] != -4) {
        return false;
    }

    auto emptyAwaiter = channel.recv_batch_to(std::span<Pointer>{});
    if (!emptyAwaiter.await_ready()) {
        return false;
    }
    auto emptyResult = emptyAwaiter.await_resume();
    if (!emptyResult.has_value() || *emptyResult != 0) {
        return false;
    }

    std::array<Pointer, 1> closedOutput{std::make_unique<int>(-9)};
    auto closedAwaiter =
        channel.recv_batch_to(std::span<Pointer>(closedOutput));
    if (!closedAwaiter.await_ready()) {
        return false;
    }
    auto closedResult = closedAwaiter.await_resume();
    return !closedResult.has_value() && closedOutput[0] &&
        *closedOutput[0] == -9 &&
        galay::kernel::IOError::contains(
            closedResult.error().code(), galay::kernel::kClosed);
}

bool run_static_spsc_channel()
{
    StaticSpscChannel channel;
    if (channel.error() != galay::spsc::RingError::kNone ||
        channel.capacity() != 8) {
        return false;
    }
    for (int value = 0; value < 8; ++value) {
        if (!channel.try_send(value)) {
            return false;
        }
    }
    if (channel.try_send(8)) {
        return false;
    }
    for (int expected = 0; expected < 8; ++expected) {
        auto value = channel.try_recv();
        if (!value.has_value() || *value != expected) {
            return false;
        }
    }

    StaticSpscChannel waitChannel;
    galay::kernel::ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    AsyncState state;
    if (!galay::kernel::schedule_task(
            scheduler, receive_one(&waitChannel, &state)) ||
        !wait_for(state.entered) || state.done.load(std::memory_order_acquire)) {
        scheduler.stop();
        return false;
    }
    const bool sent = waitChannel.try_send(91);
    const bool done = wait_for(state.done);
    scheduler.stop();
    return sent && done && state.success && state.value == 91 &&
        waitChannel.empty();
}

bool run_mpsc_small_capacity()
{
    constexpr uint32_t kProducerCount = 4;
    constexpr uint32_t kMessagesPerProducer = 50'000;
    constexpr uint64_t kMessageCount =
        static_cast<uint64_t>(kProducerCount) * kMessagesPerProducer;
    galay::mpsc::BoundedChannel<uint64_t> channel(2);
    galay::benchmark::CompletionLatch ready(kProducerCount + 1);
    galay::benchmark::StartGate start;
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> received{0};
    const auto deadline = std::chrono::steady_clock::now() + 10s;

    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (uint32_t producerId = 0; producerId < kProducerCount; ++producerId) {
        producers.emplace_back([&, producerId]() {
            ready.arrive();
            start.wait();
            for (uint32_t sequence = 0; sequence < kMessagesPerProducer; ++sequence) {
                uint64_t value = (static_cast<uint64_t>(producerId) << 32U) | sequence;
                while (!channel.try_send(std::move(value))) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }

    std::thread consumer([&]() {
        ready.arrive();
        start.wait();
        std::array<uint32_t, kProducerCount> expected{};
        uint64_t receivedCount = 0;
        while (receivedCount < kMessageCount) {
            auto value = channel.try_recv();
            if (!value.has_value()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
                continue;
            }
            const uint32_t producerId = static_cast<uint32_t>(*value >> 32U);
            const uint32_t sequence = static_cast<uint32_t>(*value);
            if (producerId >= kProducerCount || sequence != expected[producerId]) {
                failed.store(true, std::memory_order_release);
                return;
            }
            ++expected[producerId];
            ++receivedCount;
            received.store(receivedCount, std::memory_order_release);
        }
    });

    const bool allReady = ready.wait_for(2s);
    start.open();
    for (auto& producer : producers) {
        producer.join();
    }
    consumer.join();
    return allReady && !failed.load(std::memory_order_acquire) && channel.empty() &&
        received.load(std::memory_order_acquire) == kMessageCount;
}

} // namespace

int main()
{
    galay::test::TestResultWriter writer("t152_bounded_topology");
    const bool independentSources = check_independent_sources();
    const bool spscBasic = run_basic_close_and_drain<SpscChannel>();
    const bool mpscBasic = run_basic_close_and_drain<MpscChannel>();
    const bool mpmcBasic = run_basic_close_and_drain<MpmcChannel>();
    const bool spscWaiter = run_waiter_progress<SpscChannel>();
    const bool mpscWaiter = run_waiter_progress<MpscChannel>();
    const bool mpmcWaiter = run_waiter_progress<MpmcChannel>();
    const bool spscWraparound = run_spsc_wraparound();
    const bool spscCallerOwnedBatch = run_spsc_caller_owned_batch();
    const bool staticSpscChannel = run_static_spsc_channel();
    const bool mpscSmallCapacity = run_mpsc_small_capacity();
    const bool passed = independentSources && spscBasic && mpscBasic && mpmcBasic &&
        spscWaiter && mpscWaiter && mpmcWaiter && spscWraparound &&
        spscCallerOwnedBatch && staticSpscChannel && mpscSmallCapacity;

    writer.add_test();
    if (passed) {
        writer.add_passed();
    } else {
        writer.add_failed();
    }
    writer.write_result();

    std::cout << "independent_sources=" << (independentSources ? "PASS" : "FAIL") << '\n'
              << "spsc_basic=" << (spscBasic ? "PASS" : "FAIL") << '\n'
              << "mpsc_basic=" << (mpscBasic ? "PASS" : "FAIL") << '\n'
              << "mpmc_basic=" << (mpmcBasic ? "PASS" : "FAIL") << '\n'
              << "spsc_waiter=" << (spscWaiter ? "PASS" : "FAIL") << '\n'
              << "mpsc_waiter=" << (mpscWaiter ? "PASS" : "FAIL") << '\n'
              << "mpmc_waiter=" << (mpmcWaiter ? "PASS" : "FAIL") << '\n'
              << "spsc_wraparound=" << (spscWraparound ? "PASS" : "FAIL") << '\n'
              << "spsc_caller_owned_batch="
              << (spscCallerOwnedBatch ? "PASS" : "FAIL") << '\n'
              << "spsc_static_channel=" << (staticSpscChannel ? "PASS" : "FAIL")
              << '\n'
              << "mpsc_small_capacity=" << (mpscSmallCapacity ? "PASS" : "FAIL")
              << '\n';
    return passed ? 0 : 1;
}
