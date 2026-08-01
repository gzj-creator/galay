/**
 * @file t163_mpsc_redesign.cc
 * @brief 验证 MPSC 重设计后的关闭、批量排空和元素类型契约。
 */

#include "result_writer.h"
#include "test/cpp/common/mpsc_access.h"

#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <coroutine>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

struct ThrowingMove
{
    ThrowingMove() = default;
    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove& operator=(const ThrowingMove&) = delete;
    ThrowingMove(ThrowingMove&&) noexcept(false) {}
    ThrowingMove& operator=(ThrowingMove&&) noexcept(false) { return *this; }
};

static_assert(!galay::mpsc::UnboundedValue<ThrowingMove>);

struct NothrowMoveConstructOnly
{
    NothrowMoveConstructOnly() = default;
    NothrowMoveConstructOnly(const NothrowMoveConstructOnly&) = delete;
    NothrowMoveConstructOnly& operator=(
        const NothrowMoveConstructOnly&) = delete;
    NothrowMoveConstructOnly(NothrowMoveConstructOnly&&) noexcept = default;
    NothrowMoveConstructOnly& operator=(
        NothrowMoveConstructOnly&&) noexcept(false)
    {
        return *this;
    }
};

static_assert(galay::mpsc::UnboundedValue<NothrowMoveConstructOnly>);

using UIntChannel = galay::mpsc::UnboundedChannel<uint64_t>;
using UIntRecvAwaitable = galay::mpsc::UnboundedRecvAwaitable<uint64_t>;
using UIntBatchAwaitable = galay::mpsc::UnboundedRecvBatchAwaitable<uint64_t>;
using UIntBatchToAwaitable = galay::mpsc::UnboundedRecvBatchToAwaitable<uint64_t>;
using ConstructOnlyRecvAwaitable =
    galay::mpsc::UnboundedRecvAwaitable<NothrowMoveConstructOnly>;
struct AwaitSuspendProbePromise {};
static_assert(noexcept(std::declval<UIntChannel&>().send(uint64_t{})));
static_assert(noexcept(std::declval<UIntChannel&>().sendBatch(
    std::declval<std::vector<uint64_t>&&>())));
static_assert(noexcept(std::declval<UIntChannel&>().tryRecv()));
static_assert(!std::is_constructible_v<UIntRecvAwaitable, UIntChannel*>);
static_assert(!std::is_copy_constructible_v<UIntRecvAwaitable>);
static_assert(std::is_nothrow_move_constructible_v<UIntRecvAwaitable>);
static_assert(std::is_nothrow_move_assignable_v<UIntRecvAwaitable>);
static_assert(std::movable<ConstructOnlyRecvAwaitable>);
static_assert(std::is_nothrow_move_constructible_v<ConstructOnlyRecvAwaitable>);
static_assert(std::is_nothrow_move_assignable_v<ConstructOnlyRecvAwaitable>);
static_assert(!std::is_constructible_v<UIntBatchAwaitable,
                                       UIntChannel*,
                                       size_t>);
static_assert(!std::is_copy_constructible_v<UIntBatchAwaitable>);
static_assert(std::is_nothrow_move_constructible_v<UIntBatchAwaitable>);
static_assert(std::is_nothrow_move_assignable_v<UIntBatchAwaitable>);
static_assert(!noexcept(std::declval<UIntBatchAwaitable&>().await_ready()));
static_assert(!noexcept(std::declval<UIntBatchAwaitable&>().await_suspend(
    std::coroutine_handle<AwaitSuspendProbePromise>{})));
static_assert(!noexcept(std::declval<UIntBatchAwaitable&>().await_resume()));
static_assert(!std::is_constructible_v<UIntBatchToAwaitable,
                                       UIntChannel*,
                                       std::vector<uint64_t>*,
                                       size_t>);
static_assert(!std::is_copy_constructible_v<UIntBatchToAwaitable>);
static_assert(std::is_nothrow_move_constructible_v<UIntBatchToAwaitable>);
static_assert(std::is_nothrow_move_assignable_v<UIntBatchToAwaitable>);

struct MoveGate
{
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

struct BlockingMove
{
    uint64_t value = 0;
    MoveGate* gate = nullptr;
    bool block = false;

    BlockingMove(uint64_t input, MoveGate* inputGate, bool shouldBlock) noexcept
        : value(input), gate(inputGate), block(shouldBlock)
    {
    }
    BlockingMove(const BlockingMove&) = delete;
    BlockingMove& operator=(const BlockingMove&) = delete;
    BlockingMove(BlockingMove&& other) noexcept
        : value(other.value), gate(other.gate), block(false)
    {
        if (std::exchange(other.block, false) && gate != nullptr) {
            gate->entered.store(true, std::memory_order_release);
            gate->entered.notify_all();
            gate->release.wait(false, std::memory_order_acquire);
        }
    }
    BlockingMove& operator=(BlockingMove&& other) noexcept
    {
        value = other.value;
        gate = other.gate;
        block = false;
        other.block = false;
        return *this;
    }
};

bool testCloseDrainsPublishedValues()
{
    galay::mpsc::UnboundedChannel<uint64_t> channel;
    auto first = channel.makeProducerToken();
    auto second = channel.makeProducerToken();
    if (!first.valid() || !second.valid()) {
        return false;
    }

    if (!channel.send(first, 11) || !channel.send(second, 22)) {
        return false;
    }
    if (!channel.close() || !channel.isClosed()) {
        return false;
    }

    uint64_t rejected = 33;
    if (channel.send(first, std::move(rejected))) {
        return false;
    }

    auto one = channel.tryRecv();
    auto two = channel.tryRecv();
    return one.has_value() && two.has_value() &&
        ((*one == 11 && *two == 22) || (*one == 22 && *two == 11)) &&
        !channel.tryRecv().has_value() && channel.isClosedAndDrained();
}

bool testDrainToReusesCapacity()
{
    galay::mpsc::UnboundedChannel<uint64_t> channel;
    auto token = channel.makeProducerToken();
    if (!token.valid()) {
        return false;
    }
    for (uint64_t value = 0; value < 32; ++value) {
        if (!channel.send(token, std::move(value))) {
            return false;
        }
    }

    std::vector<uint64_t> values;
    values.reserve(16);
    const size_t initialCapacity = values.capacity();
    const size_t drained = channel.drainTo(values, 32);
    if (drained != 16 || values.size() != 16 ||
        values.capacity() != initialCapacity) {
        return false;
    }
    for (uint64_t value = 0; value < values.size(); ++value) {
        if (values[value] != value) {
            return false;
        }
    }
    return channel.size() == 16;
}

bool testClosedReceiveCompletesWithoutSuspending()
{
    galay::mpsc::UnboundedChannel<uint64_t> channel;
    if (!channel.close()) {
        return false;
    }
    auto receive = channel.recv();
    if (!receive.await_ready()) {
        return false;
    }
    auto result = receive.await_resume();
    return !result.has_value() &&
        galay::kernel::IOError::contains(result.error().code(),
                                        galay::kernel::kClosed);
}

bool testCloseWaitsForInFlightSends()
{
    using Channel = galay::mpsc::UnboundedChannel<BlockingMove>;
    constexpr size_t kProducerCount = 4;
    Channel channel;
    std::vector<Channel::ProducerToken> tokens;
    tokens.reserve(kProducerCount);
    for (size_t producer = 0; producer < kProducerCount; ++producer) {
        auto token = channel.makeProducerToken();
        if (!token.valid()) {
            return false;
        }
        tokens.push_back(std::move(token));
    }

    std::array<MoveGate, kProducerCount> gates;
    std::array<std::atomic<bool>, kProducerCount> sent{};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (size_t producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            BlockingMove value(91 + producer, &gates[producer], true);
            sent[producer].store(
                channel.send(tokens[producer], std::move(value)),
                std::memory_order_release);
        });
    }
    for (MoveGate& gate : gates) {
        gate.entered.wait(false, std::memory_order_acquire);
    }

    std::atomic<bool> closeReturned{false};
    std::atomic<bool> closeSucceeded{false};
    std::thread closer([&]() {
        closeSucceeded.store(channel.close(), std::memory_order_release);
        closeReturned.store(true, std::memory_order_release);
    });
    while (!channel.isClosed()) {
        std::this_thread::yield();
    }
    for (size_t producer = 0; producer + 1 < kProducerCount; ++producer) {
        gates[producer].release.store(true, std::memory_order_release);
        gates[producer].release.notify_all();
        producers[producer].join();
    }
    const bool closeWaitedForLastProducer =
        !closeReturned.load(std::memory_order_acquire) &&
        !channel.isClosedAndDrained();

    gates.back().release.store(true, std::memory_order_release);
    gates.back().release.notify_all();
    producers.back().join();
    closer.join();

    std::array<bool, kProducerCount> received{};
    for (size_t count = 0; count < kProducerCount; ++count) {
        auto value = channel.tryRecv();
        if (!value.has_value() || value->value < 91 ||
            value->value >= 91 + kProducerCount) {
            return false;
        }
        const size_t producer = value->value - 91;
        if (received[producer]) {
            return false;
        }
        received[producer] = true;
    }
    auto lateToken = channel.makeProducerToken();
    bool allSent = true;
    for (const std::atomic<bool>& producerSent : sent) {
        allSent = allSent && producerSent.load(std::memory_order_acquire);
    }
    return closeWaitedForLastProducer && allSent &&
        closeSucceeded.load(std::memory_order_acquire) &&
        closeReturned.load(std::memory_order_acquire) &&
        !channel.tryRecv().has_value() && channel.isClosedAndDrained() &&
        !lateToken.valid();
}

bool testSendStartingAfterCloseBeginsFails()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    Channel channel;
    auto token = channel.makeProducerToken();
    if (!token.valid()) {
        return false;
    }

    galay::mpsc::UnboundedChannelTestAccess::holdProducerRegistration(channel);
    std::atomic<bool> closeSucceeded{false};
    std::thread closer([&]() {
        closeSucceeded.store(channel.close(), std::memory_order_release);
    });
    while (!channel.isClosed()) {
        std::this_thread::yield();
    }

    uint64_t value = 7;
    const bool sentAfterClosing = channel.send(token, std::move(value));
    galay::mpsc::UnboundedChannelTestAccess::releaseProducerRegistration(channel);
    closer.join();
    return !sentAfterClosing && closeSucceeded.load(std::memory_order_acquire) &&
        channel.isClosedAndDrained();
}

bool testConcurrentCloseLoserReturnsImmediately()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    Channel channel;
    galay::mpsc::UnboundedChannelTestAccess::holdProducerRegistration(channel);

    std::atomic<bool> winnerSucceeded{false};
    std::thread winner([&]() {
        winnerSucceeded.store(channel.close(), std::memory_order_release);
    });
    const auto closingDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!channel.isClosed() &&
           std::chrono::steady_clock::now() < closingDeadline) {
        std::this_thread::yield();
    }
    if (!channel.isClosed()) {
        galay::mpsc::UnboundedChannelTestAccess::releaseProducerRegistration(channel);
        winner.join();
        return false;
    }

    std::atomic<bool> loserReturned{false};
    std::atomic<bool> loserSucceeded{true};
    std::thread loser([&]() {
        loserSucceeded.store(channel.close(), std::memory_order_release);
        loserReturned.store(true, std::memory_order_release);
    });
    const auto loserDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!loserReturned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < loserDeadline) {
        std::this_thread::yield();
    }
    const bool returnedBeforeWinner =
        loserReturned.load(std::memory_order_acquire);

    galay::mpsc::UnboundedChannelTestAccess::releaseProducerRegistration(channel);
    loser.join();
    winner.join();
    return returnedBeforeWinner &&
        !loserSucceeded.load(std::memory_order_acquire) &&
        winnerSucceeded.load(std::memory_order_acquire);
}

bool testEmptyBatchHonorsValidityAndClose()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    Channel channel;
    auto token = channel.makeProducerToken();
    if (!token.valid()) {
        return false;
    }
    auto invalid = std::move(token);
    std::vector<uint64_t> empty;
    if (channel.sendBatch(token, empty) ||
        !channel.sendBatch(invalid, empty) ||
        !channel.sendBatch(empty)) {
        return false;
    }
    if (!channel.close()) {
        return false;
    }
    return !channel.sendBatch(invalid, empty) && !channel.sendBatch(empty);
}

bool testUnlimitedBatchLimitDoesNotTerminate()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    constexpr size_t kUnlimited = std::numeric_limits<size_t>::max();
    Channel channel;

    if (channel.tryRecvBatch(kUnlimited).has_value()) {
        return false;
    }
    auto emptyWait = channel.recvBatch(kUnlimited);
    if (emptyWait.await_ready()) {
        return false;
    }

    if (!channel.send(42)) {
        return false;
    }
    auto receive = channel.recvBatch(kUnlimited);
    if (!receive.await_ready()) {
        return false;
    }
    auto result = receive.await_resume();
    return result.has_value() && result->size() == 1 && result->front() == 42;
}

bool testPreallocatedBatchAwaitable()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    constexpr size_t kUnlimited = std::numeric_limits<size_t>::max();
    Channel channel;
    for (uint64_t value = 1; value <= 3; ++value) {
        if (!channel.send(value)) {
            return false;
        }
    }

    std::vector<uint64_t> destination;
    destination.reserve(2);
    auto receive = channel.recvBatchTo(destination, kUnlimited);
    if (!receive.await_ready()) {
        return false;
    }
    auto received = receive.await_resume();
    if (!received.has_value() || *received != 2 || destination.size() != 2 ||
        destination[0] != 1 || destination[1] != 2) {
        return false;
    }

    std::vector<uint64_t> noCapacity;
    auto invalid = channel.recvBatchTo(noCapacity, 1);
    if (!invalid.await_ready()) {
        return false;
    }
    auto invalidResult = invalid.await_resume();
    if (invalidResult.has_value() ||
        !galay::kernel::IOError::contains(invalidResult.error().code(),
                                         galay::kernel::kParamInvalid)) {
        return false;
    }

    if (!channel.close()) {
        return false;
    }
    auto zero = channel.recvBatchTo(noCapacity, 0);
    if (!zero.await_ready()) {
        return false;
    }
    auto zeroResult = zero.await_resume();
    return zeroResult.has_value() && *zeroResult == 0;
}

bool testCounterBoundary(uint64_t boundary)
{
    UIntChannel channel(UIntChannel::DEFAULT_BATCH_SIZE, 0);
    auto token = channel.makeProducerToken();
    if (!token.valid() || boundary == 0 ||
        !galay::mpsc::UnboundedChannelTestAccess::seedOnlyStreamSequence(
            channel, boundary - 1)) {
        return false;
    }

    uint64_t first = 101;
    if (!channel.send(token, std::move(first)) || channel.size() != 1) {
        return false;
    }
    auto firstBatch = channel.tryRecvBatch(1);
    if (!firstBatch.has_value() || firstBatch->size() != 1 ||
        firstBatch->front() != 101 || !channel.empty()) {
        return false;
    }

    uint64_t second = 202;
    if (!channel.send(token, std::move(second)) || channel.size() != 1) {
        return false;
    }
    auto secondBatch = channel.tryRecvBatch(1);
    if (!secondBatch.has_value() || secondBatch->size() != 1 ||
        secondBatch->front() != 202 || !channel.empty()) {
        return false;
    }

    return channel.close() && channel.isClosedAndDrained();
}

bool testCumulativeCountersCross32BitBoundary()
{
    return testCounterBoundary(std::numeric_limits<uint32_t>::max());
}

bool testCumulativeCountersWrapModulo64()
{
    return testCounterBoundary(std::numeric_limits<uint64_t>::max());
}

bool testSizeSnapshotSaturates()
{
    UIntChannel channel(UIntChannel::DEFAULT_BATCH_SIZE, 0);
    auto first = channel.makeProducerToken();
    auto second = channel.makeProducerToken();
    auto third = channel.makeProducerToken();
    if (!first.valid() || !second.valid() || !third.valid()) {
        return false;
    }

    constexpr uint64_t kSyntheticPending =
        std::numeric_limits<uint64_t>::max() / 3 + 1;
    const size_t seeded =
        galay::mpsc::UnboundedChannelTestAccess::setSyntheticPendingForAllStreams(
            channel, kSyntheticPending);
    const size_t snapshot = channel.size();
    const size_t reset =
        galay::mpsc::UnboundedChannelTestAccess::setSyntheticPendingForAllStreams(
            channel, 0);

    return seeded == 3 && reset == seeded &&
        snapshot == std::numeric_limits<size_t>::max() && channel.empty() &&
        channel.close() && channel.isClosedAndDrained();
}

bool testInconsistentSizeSnapshotDoesNotExplode()
{
    UIntChannel channel(UIntChannel::DEFAULT_BATCH_SIZE, 0);
    auto token = channel.makeProducerToken();
    if (!token.valid() ||
        !galay::mpsc::UnboundedChannelTestAccess::setOnlyStreamObservedCounters(
            channel, 100, 101)) {
        return false;
    }

    const size_t snapshot = channel.size();
    const bool reset =
        galay::mpsc::UnboundedChannelTestAccess::setOnlyStreamObservedCounters(
            channel, 0, 0);
    return reset && snapshot == 0 && channel.empty() && channel.close();
}

} // namespace

int main()
{
    galay::test::TestResultWriter writer("t163_mpsc_redesign");
    writer.addTest();

    bool passed = true;
    const auto check = [&passed](bool result, const char* name) {
        if (!result) {
            std::cerr << name << " failed\n";
            passed = false;
        }
    };
    check(testCloseDrainsPublishedValues(), "close drains published values");
    check(testDrainToReusesCapacity(), "drainTo reuses capacity");
    check(testClosedReceiveCompletesWithoutSuspending(),
          "closed receive completes without suspending");
    check(testCloseWaitsForInFlightSends(), "close waits for in-flight sends");
    check(testSendStartingAfterCloseBeginsFails(),
          "send starting after close begins fails");
    check(testConcurrentCloseLoserReturnsImmediately(),
          "concurrent close loser returns immediately");
    check(testEmptyBatchHonorsValidityAndClose(),
          "empty batch honors validity and close");
    check(testUnlimitedBatchLimitDoesNotTerminate(),
          "unlimited batch limit does not terminate");
    check(testPreallocatedBatchAwaitable(), "preallocated batch awaitable");
    check(testCumulativeCountersCross32BitBoundary(),
          "cumulative counters cross 32-bit boundary");
    check(testCumulativeCountersWrapModulo64(),
          "cumulative counters wrap modulo 64");
    check(testSizeSnapshotSaturates(), "size snapshot saturates");
    check(testInconsistentSizeSnapshotDoesNotExplode(),
          "inconsistent size snapshot does not explode");
    if (!passed) {
        std::cerr << "MPSC redesign boundary test failed\n";
        writer.addFailed();
        writer.writeResult();
        return 1;
    }

    writer.addPassed();
    writer.writeResult();
    std::cout << "t163_mpsc_redesign PASS\n";
    return 0;
}
