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
static_assert(noexcept(std::declval<UIntChannel&>().send_batch(
    std::declval<std::vector<uint64_t>&&>())));
static_assert(noexcept(std::declval<UIntChannel&>().try_recv()));
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

bool wait_for_flag(const std::atomic<bool>& flag) noexcept
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!flag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return flag.load(std::memory_order_acquire);
}

bool test_close_drains_published_values()
{
    galay::mpsc::UnboundedChannel<uint64_t> channel;
    auto first = channel.make_producer_token();
    auto second = channel.make_producer_token();
    if (!first.valid() || !second.valid()) {
        return false;
    }

    if (!channel.send(first, 11) || !channel.send(second, 22)) {
        return false;
    }
    if (!channel.close() || !channel.is_closed()) {
        return false;
    }

    uint64_t rejected = 33;
    if (channel.send(first, std::move(rejected))) {
        return false;
    }

    auto one = channel.try_recv();
    auto two = channel.try_recv();
    return one.has_value() && two.has_value() &&
        ((*one == 11 && *two == 22) || (*one == 22 && *two == 11)) &&
        !channel.try_recv().has_value() && channel.is_closed_and_drained();
}

bool test_drain_to_reuses_capacity()
{
    galay::mpsc::UnboundedChannel<uint64_t> channel;
    auto token = channel.make_producer_token();
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
    const size_t drained = channel.drain_to(values, 32);
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

bool test_closed_receive_completes_without_suspending()
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

bool test_close_waits_for_in_flight_sends()
{
    using Channel = galay::mpsc::UnboundedChannel<BlockingMove>;
    constexpr size_t kProducerCount = 4;
    Channel channel;
    std::vector<Channel::ProducerToken> tokens;
    tokens.reserve(kProducerCount);
    for (size_t producer = 0; producer < kProducerCount; ++producer) {
        auto token = channel.make_producer_token();
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
    while (!channel.is_closed()) {
        std::this_thread::yield();
    }
    for (size_t producer = 0; producer + 1 < kProducerCount; ++producer) {
        gates[producer].release.store(true, std::memory_order_release);
        gates[producer].release.notify_all();
        producers[producer].join();
    }
    const bool closeWaitedForLastProducer =
        !closeReturned.load(std::memory_order_acquire) &&
        !channel.is_closed_and_drained();

    gates.back().release.store(true, std::memory_order_release);
    gates.back().release.notify_all();
    producers.back().join();
    closer.join();

    std::array<bool, kProducerCount> received{};
    for (size_t count = 0; count < kProducerCount; ++count) {
        auto value = channel.try_recv();
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
    auto lateToken = channel.make_producer_token();
    bool allSent = true;
    for (const std::atomic<bool>& producerSent : sent) {
        allSent = allSent && producerSent.load(std::memory_order_acquire);
    }
    return closeWaitedForLastProducer && allSent &&
        closeSucceeded.load(std::memory_order_acquire) &&
        closeReturned.load(std::memory_order_acquire) &&
        !channel.try_recv().has_value() && channel.is_closed_and_drained() &&
        !lateToken.valid();
}

bool test_batch_publication_remains_atomic_across_close()
{
    using Channel = galay::mpsc::UnboundedChannel<BlockingMove>;
    Channel channel;
    auto token = channel.make_producer_token();
    if (!token.valid()) {
        return false;
    }

    MoveGate gate;
    std::vector<BlockingMove> values;
    values.reserve(3);
    values.emplace_back(101, nullptr, false);
    values.emplace_back(102, &gate, true);
    values.emplace_back(103, nullptr, false);

    std::atomic<bool> sendSucceeded{false};
    std::thread producer([&]() {
        sendSucceeded.store(
            channel.send_batch(token, std::move(values)),
            std::memory_order_release);
    });

    if (!wait_for_flag(gate.entered)) {
        gate.release.store(true, std::memory_order_release);
        gate.release.notify_all();
        producer.join();
        const bool closed = channel.close();
        if (!closed) {
            return false;
        }
        return false;
    }

    std::atomic<bool> closeReturned{false};
    std::atomic<bool> closeSucceeded{false};
    std::thread closer([&]() {
        closeSucceeded.store(channel.close(), std::memory_order_release);
        closeReturned.store(true, std::memory_order_release);
    });

    const auto closingDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!channel.is_closed() &&
           std::chrono::steady_clock::now() < closingDeadline) {
        std::this_thread::yield();
    }
    const bool closeStarted = channel.is_closed();
    auto hiddenValue = channel.try_recv();
    const bool batchStayedHidden = !hiddenValue.has_value() && channel.empty();
    const bool closeWaitedForWholeBatch =
        !closeReturned.load(std::memory_order_acquire);

    gate.release.store(true, std::memory_order_release);
    gate.release.notify_all();
    producer.join();
    closer.join();

    auto first = channel.try_recv();
    auto second = channel.try_recv();
    auto third = channel.try_recv();
    auto extra = channel.try_recv();
    return closeStarted && batchStayedHidden && closeWaitedForWholeBatch &&
        sendSucceeded.load(std::memory_order_acquire) &&
        closeSucceeded.load(std::memory_order_acquire) &&
        closeReturned.load(std::memory_order_acquire) && first.has_value() &&
        first->value == 101 && second.has_value() && second->value == 102 &&
        third.has_value() && third->value == 103 && !extra.has_value() &&
        channel.is_closed_and_drained();
}

bool test_send_starting_after_close_begins_fails()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    Channel channel;
    auto token = channel.make_producer_token();
    if (!token.valid()) {
        return false;
    }

    galay::mpsc::UnboundedChannelTestAccess::hold_producer_registration(channel);
    std::atomic<bool> closeSucceeded{false};
    std::thread closer([&]() {
        closeSucceeded.store(channel.close(), std::memory_order_release);
    });
    while (!channel.is_closed()) {
        std::this_thread::yield();
    }

    uint64_t value = 7;
    const bool sentAfterClosing = channel.send(token, std::move(value));
    galay::mpsc::UnboundedChannelTestAccess::release_producer_registration(channel);
    closer.join();
    return !sentAfterClosing && closeSucceeded.load(std::memory_order_acquire) &&
        channel.is_closed_and_drained();
}

bool test_concurrent_close_loser_returns_immediately()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    Channel channel;
    galay::mpsc::UnboundedChannelTestAccess::hold_producer_registration(channel);

    std::atomic<bool> winnerSucceeded{false};
    std::thread winner([&]() {
        winnerSucceeded.store(channel.close(), std::memory_order_release);
    });
    const auto closingDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!channel.is_closed() &&
           std::chrono::steady_clock::now() < closingDeadline) {
        std::this_thread::yield();
    }
    if (!channel.is_closed()) {
        galay::mpsc::UnboundedChannelTestAccess::release_producer_registration(channel);
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

    galay::mpsc::UnboundedChannelTestAccess::release_producer_registration(channel);
    loser.join();
    winner.join();
    return returnedBeforeWinner &&
        !loserSucceeded.load(std::memory_order_acquire) &&
        winnerSucceeded.load(std::memory_order_acquire);
}

bool test_empty_batch_honors_validity_and_close()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    Channel channel;
    auto token = channel.make_producer_token();
    if (!token.valid()) {
        return false;
    }
    auto invalid = std::move(token);
    std::vector<uint64_t> empty;
    if (channel.send_batch(token, empty) ||
        !channel.send_batch(invalid, empty) ||
        !channel.send_batch(empty)) {
        return false;
    }
    if (!channel.close()) {
        return false;
    }
    return !channel.send_batch(invalid, empty) && !channel.send_batch(empty);
}

bool test_unlimited_batch_limit_does_not_terminate()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    constexpr size_t kUnlimited = std::numeric_limits<size_t>::max();
    Channel channel;

    if (channel.try_recv_batch(kUnlimited).has_value()) {
        return false;
    }
    auto emptyWait = channel.recv_batch(kUnlimited);
    if (emptyWait.await_ready()) {
        return false;
    }

    if (!channel.send(42)) {
        return false;
    }
    auto receive = channel.recv_batch(kUnlimited);
    if (!receive.await_ready()) {
        return false;
    }
    auto result = receive.await_resume();
    return result.has_value() && result->size() == 1 && result->front() == 42;
}

bool test_preallocated_batch_awaitable()
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
    auto receive = channel.recv_batch_to(destination, kUnlimited);
    if (!receive.await_ready()) {
        return false;
    }
    auto received = receive.await_resume();
    if (!received.has_value() || *received != 2 || destination.size() != 2 ||
        destination[0] != 1 || destination[1] != 2) {
        return false;
    }

    std::vector<uint64_t> noCapacity;
    auto invalid = channel.recv_batch_to(noCapacity, 1);
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
    auto zero = channel.recv_batch_to(noCapacity, 0);
    if (!zero.await_ready()) {
        return false;
    }
    auto zeroResult = zero.await_resume();
    return zeroResult.has_value() && *zeroResult == 0;
}

bool test_counter_boundary(uint64_t boundary)
{
    UIntChannel channel(UIntChannel::DEFAULT_BATCH_SIZE, 0);
    auto token = channel.make_producer_token();
    if (!token.valid() || boundary == 0 ||
        !galay::mpsc::UnboundedChannelTestAccess::seed_only_stream_sequence(
            channel, boundary - 1)) {
        return false;
    }

    uint64_t first = 101;
    if (!channel.send(token, std::move(first)) || channel.size() != 1) {
        return false;
    }
    auto firstBatch = channel.try_recv_batch(1);
    if (!firstBatch.has_value() || firstBatch->size() != 1 ||
        firstBatch->front() != 101 || !channel.empty()) {
        return false;
    }

    uint64_t second = 202;
    if (!channel.send(token, std::move(second)) || channel.size() != 1) {
        return false;
    }
    auto secondBatch = channel.try_recv_batch(1);
    if (!secondBatch.has_value() || secondBatch->size() != 1 ||
        secondBatch->front() != 202 || !channel.empty()) {
        return false;
    }

    return channel.close() && channel.is_closed_and_drained();
}

bool test_cumulative_counters_cross32_bit_boundary()
{
    return test_counter_boundary(std::numeric_limits<uint32_t>::max());
}

bool test_cumulative_counters_wrap_modulo64()
{
    return test_counter_boundary(std::numeric_limits<uint64_t>::max());
}

bool test_size_snapshot_saturates()
{
    UIntChannel channel(UIntChannel::DEFAULT_BATCH_SIZE, 0);
    auto first = channel.make_producer_token();
    auto second = channel.make_producer_token();
    auto third = channel.make_producer_token();
    if (!first.valid() || !second.valid() || !third.valid()) {
        return false;
    }

    constexpr uint64_t kSyntheticPending =
        std::numeric_limits<uint64_t>::max() / 3 + 1;
    const size_t seeded =
        galay::mpsc::UnboundedChannelTestAccess::set_synthetic_pending_for_all_streams(
            channel, kSyntheticPending);
    const size_t snapshot = channel.size();
    const size_t reset =
        galay::mpsc::UnboundedChannelTestAccess::set_synthetic_pending_for_all_streams(
            channel, 0);

    return seeded == 3 && reset == seeded &&
        snapshot == std::numeric_limits<size_t>::max() && channel.empty() &&
        channel.close() && channel.is_closed_and_drained();
}

bool test_inconsistent_size_snapshot_does_not_explode()
{
    UIntChannel channel(UIntChannel::DEFAULT_BATCH_SIZE, 0);
    auto token = channel.make_producer_token();
    if (!token.valid() ||
        !galay::mpsc::UnboundedChannelTestAccess::set_only_stream_observed_counters(
            channel, 100, 101)) {
        return false;
    }

    const size_t snapshot = channel.size();
    const bool reset =
        galay::mpsc::UnboundedChannelTestAccess::set_only_stream_observed_counters(
            channel, 0, 0);
    return reset && snapshot == 0 && channel.empty() && channel.close();
}

} // namespace

int main()
{
    galay::test::TestResultWriter writer("t163_mpsc_redesign");
    writer.add_test();

    bool passed = true;
    const auto check = [&passed](bool result, const char* name) {
        if (!result) {
            std::cerr << name << " failed\n";
            passed = false;
        }
    };
    check(test_close_drains_published_values(), "close drains published values");
    check(test_drain_to_reuses_capacity(), "drainTo reuses capacity");
    check(test_closed_receive_completes_without_suspending(),
          "closed receive completes without suspending");
    check(test_close_waits_for_in_flight_sends(), "close waits for in-flight sends");
    check(test_batch_publication_remains_atomic_across_close(),
          "batch publication remains atomic across close");
    check(test_send_starting_after_close_begins_fails(),
          "send starting after close begins fails");
    check(test_concurrent_close_loser_returns_immediately(),
          "concurrent close loser returns immediately");
    check(test_empty_batch_honors_validity_and_close(),
          "empty batch honors validity and close");
    check(test_unlimited_batch_limit_does_not_terminate(),
          "unlimited batch limit does not terminate");
    check(test_preallocated_batch_awaitable(), "preallocated batch awaitable");
    check(test_cumulative_counters_cross32_bit_boundary(),
          "cumulative counters cross 32-bit boundary");
    check(test_cumulative_counters_wrap_modulo64(),
          "cumulative counters wrap modulo 64");
    check(test_size_snapshot_saturates(), "size snapshot saturates");
    check(test_inconsistent_size_snapshot_does_not_explode(),
          "inconsistent size snapshot does not explode");
    if (!passed) {
        std::cerr << "MPSC redesign boundary test failed\n";
        writer.add_failed();
        writer.write_result();
        return 1;
    }

    writer.add_passed();
    writer.write_result();
    std::cout << "t163_mpsc_redesign PASS\n";
    return 0;
}
