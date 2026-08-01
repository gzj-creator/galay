/**
 * @file t152_bounded_topology.cc
 * @brief 验证有界 SPSC/MPSC/MPMC channel 的独立实现与公开拓扑语义。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/mpsc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>
#include <galay/cpp/galay-kernel/core/compute_scheduler.h>
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
    { channel.tryRecvBatch(output) } noexcept -> std::same_as<size_t>;
    { channel.recvBatchTo(output) } noexcept;
};

static_assert(!std::is_same_v<SpscChannel, MpscChannel>);
static_assert(!std::is_same_v<SpscChannel, MpmcChannel>);
static_assert(!std::is_same_v<MpscChannel, MpmcChannel>);
static_assert(std::is_nothrow_default_constructible_v<StaticSpscChannel>);
static_assert(!std::is_constructible_v<StaticSpscChannel, size_t>);
static_assert(HasCallerOwnedSpscBatch<int>);
static_assert(!HasCallerOwnedSpscBatch<PotentiallyThrowingMoveAssign>);

using SpscBatchToAwaitable = decltype(
    std::declval<SpscChannel&>().recvBatchTo(std::declval<std::span<int>>()));
using SpscVectorBatchAwaitable = decltype(
    std::declval<SpscChannel&>().recvBatch(size_t{2}));

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

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool checkIndependentSources()
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
        const std::string content = readAll(header);
        if (content.empty() || content.find("Topology") != std::string::npos ||
            content.find("concurrency/detail") != std::string::npos) {
            std::cerr << "channel source is not independent: " << header << '\n';
            return false;
        }
    }

    for (const auto& header : {headers[0], headers[1], headers[2], headers[3]}) {
        const std::string content = readAll(header);
        if (content.find("moodycamel") != std::string::npos) {
            std::cerr << "topology-specialized channel still uses MPMC queue: "
                      << header << '\n';
            return false;
        }
    }
    return true;
}

bool waitFor(const std::atomic<bool>& flag, std::chrono::milliseconds timeout = 5s)
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
galay::kernel::Task<void> sendOne(Channel* channel, AsyncState* state, int value)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->send(std::move(value));
    state->success = result.has_value();
    state->done.store(true, std::memory_order_release);
    co_return;
}

template <typename Channel>
galay::kernel::Task<void> receiveOne(Channel* channel, AsyncState* state)
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
bool runBasicCloseAndDrain()
{
    Channel channel(2);
    if (!channel.trySend(1) || !channel.trySend(2) || channel.trySend(3)) {
        return false;
    }
    channel.close();
    channel.close();
    if (!channel.isClosed() || channel.trySend(4)) {
        return false;
    }
    auto first = channel.tryRecv();
    auto second = channel.tryRecv();
    return first.has_value() && second.has_value() && *first == 1 && *second == 2 &&
        !channel.tryRecv().has_value();
}

template <typename Channel>
bool runWaiterProgress()
{
    Channel sendChannel(2);
    if (!sendChannel.trySend(1) || !sendChannel.trySend(2)) {
        return false;
    }

    galay::kernel::ComputeScheduler sendScheduler;
    auto sendStarted = sendScheduler.start();
    if (!sendStarted) {
        return false;
    }
    AsyncState sendState;
    if (!galay::kernel::scheduleTask(
            sendScheduler, sendOne(&sendChannel, &sendState, 3)) ||
        !waitFor(sendState.entered) || sendState.done.load(std::memory_order_acquire)) {
        sendScheduler.stop();
        return false;
    }
    auto first = sendChannel.tryRecv();
    const bool sendDone = waitFor(sendState.done);
    sendScheduler.stop();
    auto second = sendChannel.tryRecv();
    auto third = sendChannel.tryRecv();
    if (!sendDone || !sendState.success || !first.has_value() || !second.has_value() ||
        !third.has_value() || *first != 1 || *second != 2 || *third != 3) {
        return false;
    }

    Channel recvChannel(2);
    galay::kernel::ComputeScheduler recvScheduler;
    auto recvStarted = recvScheduler.start();
    if (!recvStarted) {
        return false;
    }
    AsyncState recvState;
    if (!galay::kernel::scheduleTask(
            recvScheduler, receiveOne(&recvChannel, &recvState)) ||
        !waitFor(recvState.entered) || recvState.done.load(std::memory_order_acquire)) {
        recvScheduler.stop();
        return false;
    }
    int value = 7;
    const bool sent = recvChannel.trySend(std::move(value));
    const bool recvDone = waitFor(recvState.done);
    recvScheduler.stop();
    return sent && recvDone && recvState.success && recvState.value == 7;
}

bool runSpscWraparound()
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
            while (!channel.trySend(std::move(value))) {
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
            auto value = channel.tryRecv();
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

    const bool allReady = ready.waitFor(2s);
    start.open();
    producer.join();
    consumer.join();
    return allReady && !failed.load(std::memory_order_acquire) && channel.empty() &&
        produced.load(std::memory_order_acquire) == kMessageCount &&
        consumed.load(std::memory_order_acquire) == kMessageCount;
}

bool runSpscCallerOwnedBatch()
{
    using Pointer = std::unique_ptr<int>;
    galay::spsc::BoundedChannel<Pointer> channel(8);

    if (!channel.trySend(std::make_unique<int>(1)) ||
        !channel.trySend(std::make_unique<int>(2)) ||
        !channel.trySend(std::make_unique<int>(3)) ||
        !channel.trySend(std::make_unique<int>(4))) {
        return false;
    }

    std::array<Pointer, 2> firstBatch{};
    const size_t firstCount = channel.tryRecvBatch(std::span<Pointer>(firstBatch));
    if (firstCount != firstBatch.size() || !firstBatch[0] || !firstBatch[1] ||
        *firstBatch[0] != 1 || *firstBatch[1] != 2) {
        return false;
    }

    auto third = channel.tryRecv();
    auto fourth = channel.tryRecv();
    if (!third.has_value() || !fourth.has_value() || !*third || !*fourth ||
        **third != 3 || **fourth != 4 || channel.tryRecv().has_value()) {
        return false;
    }

    if (channel.tryRecvBatch(std::span<Pointer>{}) != 0 ||
        !channel.trySend(std::make_unique<int>(5)) ||
        !channel.trySend(std::make_unique<int>(6))) {
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
        channel.tryRecvBatch(std::span<Pointer>(closedBatch));
    if (closedCount != 2 || !closedBatch[0] || !closedBatch[1] ||
        !closedBatch[2] || !closedBatch[3] || *closedBatch[0] != 5 ||
        *closedBatch[1] != 6 || *closedBatch[2] != -3 ||
        *closedBatch[3] != -4) {
        return false;
    }

    auto emptyAwaiter = channel.recvBatchTo(std::span<Pointer>{});
    if (!emptyAwaiter.await_ready()) {
        return false;
    }
    auto emptyResult = emptyAwaiter.await_resume();
    if (!emptyResult.has_value() || *emptyResult != 0) {
        return false;
    }

    std::array<Pointer, 1> closedOutput{std::make_unique<int>(-9)};
    auto closedAwaiter =
        channel.recvBatchTo(std::span<Pointer>(closedOutput));
    if (!closedAwaiter.await_ready()) {
        return false;
    }
    auto closedResult = closedAwaiter.await_resume();
    return !closedResult.has_value() && closedOutput[0] &&
        *closedOutput[0] == -9 &&
        galay::kernel::IOError::contains(
            closedResult.error().code(), galay::kernel::kClosed);
}

bool runStaticSpscChannel()
{
    StaticSpscChannel channel;
    if (channel.error() != galay::spsc::RingError::kNone ||
        channel.capacity() != 8) {
        return false;
    }
    for (int value = 0; value < 8; ++value) {
        if (!channel.trySend(value)) {
            return false;
        }
    }
    if (channel.trySend(8)) {
        return false;
    }
    for (int expected = 0; expected < 8; ++expected) {
        auto value = channel.tryRecv();
        if (!value.has_value() || *value != expected) {
            return false;
        }
    }

    StaticSpscChannel waitChannel;
    galay::kernel::ComputeScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    AsyncState state;
    if (!galay::kernel::scheduleTask(
            scheduler, receiveOne(&waitChannel, &state)) ||
        !waitFor(state.entered) || state.done.load(std::memory_order_acquire)) {
        scheduler.stop();
        return false;
    }
    const bool sent = waitChannel.trySend(91);
    const bool done = waitFor(state.done);
    scheduler.stop();
    return sent && done && state.success && state.value == 91 &&
        waitChannel.empty();
}

bool runMpscSmallCapacity()
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
                while (!channel.trySend(std::move(value))) {
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
            auto value = channel.tryRecv();
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

    const bool allReady = ready.waitFor(2s);
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
    const bool independentSources = checkIndependentSources();
    const bool spscBasic = runBasicCloseAndDrain<SpscChannel>();
    const bool mpscBasic = runBasicCloseAndDrain<MpscChannel>();
    const bool mpmcBasic = runBasicCloseAndDrain<MpmcChannel>();
    const bool spscWaiter = runWaiterProgress<SpscChannel>();
    const bool mpscWaiter = runWaiterProgress<MpscChannel>();
    const bool mpmcWaiter = runWaiterProgress<MpmcChannel>();
    const bool spscWraparound = runSpscWraparound();
    const bool spscCallerOwnedBatch = runSpscCallerOwnedBatch();
    const bool staticSpscChannel = runStaticSpscChannel();
    const bool mpscSmallCapacity = runMpscSmallCapacity();
    const bool passed = independentSources && spscBasic && mpscBasic && mpmcBasic &&
        spscWaiter && mpscWaiter && mpmcWaiter && spscWraparound &&
        spscCallerOwnedBatch && staticSpscChannel && mpscSmallCapacity;

    writer.addTest();
    if (passed) {
        writer.addPassed();
    } else {
        writer.addFailed();
    }
    writer.writeResult();

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
