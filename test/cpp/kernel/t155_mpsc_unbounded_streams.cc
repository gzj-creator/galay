/**
 * @file t155_mpsc_unbounded_streams.cc
 * @brief 验证独立 MPSC 分块流的数据完整性、FIFO、公平性和 token/TLS 生命周期。
 */

#include "benchmark/cpp/common/benchmark_sync.h"
#include "result_writer.h"
#include "test/cpp/common/mpsc_access.h"

#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct NonDefaultMoveOnly
{
    explicit NonDefaultMoveOnly(uint64_t input) noexcept : value(input) {}
    NonDefaultMoveOnly() = delete;
    NonDefaultMoveOnly(const NonDefaultMoveOnly&) = delete;
    NonDefaultMoveOnly& operator=(const NonDefaultMoveOnly&) = delete;
    NonDefaultMoveOnly(NonDefaultMoveOnly&&) noexcept = default;
    NonDefaultMoveOnly& operator=(NonDefaultMoveOnly&&) noexcept = default;

    uint64_t value;
};

static_assert(galay::mpsc::UnboundedValue<NonDefaultMoveOnly>);

struct LifetimeTracked
{
    explicit LifetimeTracked(uint64_t input) noexcept : value(input)
    {
        constructions.fetch_add(1, std::memory_order_relaxed);
    }

    LifetimeTracked(const LifetimeTracked&) = delete;
    LifetimeTracked& operator=(const LifetimeTracked&) = delete;

    LifetimeTracked(LifetimeTracked&& other) noexcept : value(other.value)
    {
        constructions.fetch_add(1, std::memory_order_relaxed);
        moves.fetch_add(1, std::memory_order_relaxed);
    }

    LifetimeTracked& operator=(LifetimeTracked&& other) noexcept
    {
        value = other.value;
        moves.fetch_add(1, std::memory_order_relaxed);
        return *this;
    }

    ~LifetimeTracked() noexcept
    {
        destructions.fetch_add(1, std::memory_order_relaxed);
    }

    static void reset() noexcept
    {
        constructions.store(0, std::memory_order_relaxed);
        destructions.store(0, std::memory_order_relaxed);
        moves.store(0, std::memory_order_relaxed);
    }

    inline static std::atomic<uint64_t> constructions{0};
    inline static std::atomic<uint64_t> destructions{0};
    inline static std::atomic<uint64_t> moves{0};
    uint64_t value;
};

static_assert(galay::mpsc::UnboundedValue<LifetimeTracked>);

struct Message
{
    uint64_t sequence;
    uint32_t producer;
    bool sentinel;
};

struct ThrowingCopy
{
    ThrowingCopy() noexcept = default;
    ThrowingCopy(const ThrowingCopy&) noexcept(false);
    ThrowingCopy(ThrowingCopy&&) noexcept = default;
    ThrowingCopy& operator=(ThrowingCopy&&) noexcept = default;
    ThrowingCopy& operator=(const ThrowingCopy&) = delete;
};

template <typename Channel, typename Token, typename Values>
concept HasCopyBatch = requires(Channel& channel,
                                Token& token,
                                const Values& values) {
    channel.sendBatch(token, values);
};

using ThrowingCopyChannel = galay::mpsc::UnboundedChannel<ThrowingCopy>;
static_assert(!HasCopyBatch<ThrowingCopyChannel,
                           ThrowingCopyChannel::ProducerToken,
                           std::vector<ThrowingCopy>>);

bool testMoveOnlyAcrossBlocks()
{
    constexpr uint64_t kMessages = 20'000;
    galay::mpsc::UnboundedChannel<NonDefaultMoveOnly> channel;
    auto token = channel.makeProducerToken();
    if (!token.valid()) {
        return false;
    }

    std::vector<NonDefaultMoveOnly> values;
    values.reserve(kMessages);
    for (uint64_t value = 0; value < kMessages; ++value) {
        values.emplace_back(value);
    }
    if (!channel.sendBatch(token, std::move(values))) {
        return false;
    }

    uint64_t expected = 0;
    while (expected < kMessages) {
        auto batch = channel.tryRecvBatch(777);
        if (!batch.has_value()) {
            return false;
        }
        for (const NonDefaultMoveOnly& value : *batch) {
            if (value.value != expected) {
                return false;
            }
            ++expected;
        }
    }
    return channel.empty() && channel.size() == 0;
}

bool testNonTrivialValueLifetimeAcrossRecycleAndDestruction()
{
    using Channel = galay::mpsc::UnboundedChannel<LifetimeTracked>;
    constexpr uint64_t kInitial = 4'096;
    constexpr uint64_t kDrained = 3'072;
    constexpr uint64_t kReuse = 4'096;

    LifetimeTracked::reset();
    bool passed = true;
    {
        Channel channel;
        auto token = channel.makeProducerToken();
        if (!token.valid()) {
            passed = false;
        }

        for (uint64_t sequence = 0; passed && sequence < kInitial; ++sequence) {
            LifetimeTracked value(sequence);
            if (!channel.send(token, std::move(value))) {
                passed = false;
            }
        }
        if (passed &&
            galay::mpsc::UnboundedChannelTestAccess::allocatedBlockCount(channel) <
                2) {
            passed = false;
        }

        for (uint64_t sequence = 0; passed && sequence < kDrained; ++sequence) {
            auto received = channel.tryRecv();
            if (!received.has_value() || received->value != sequence) {
                passed = false;
            }
        }
        if (passed &&
            galay::mpsc::UnboundedChannelTestAccess::recycledBlockCount(channel) ==
                0) {
            passed = false;
        }

        for (uint64_t sequence = kInitial;
             passed && sequence < kInitial + kReuse;
             ++sequence) {
            LifetimeTracked value(sequence);
            if (!channel.send(token, std::move(value))) {
                passed = false;
            }
        }

        const uint64_t expectedTail = kInitial - kDrained + kReuse;
        if (passed && (channel.empty() || channel.size() != expectedTail)) {
            passed = false;
        }
    }

    return passed &&
        LifetimeTracked::constructions.load(std::memory_order_relaxed) ==
            LifetimeTracked::destructions.load(std::memory_order_relaxed) &&
        LifetimeTracked::moves.load(std::memory_order_relaxed) != 0;
}

bool runPerProducerFifo(bool useTokens)
{
    constexpr uint32_t kProducerCount = 4;
    constexpr uint64_t kMessagesPerProducer = 50'000;
    constexpr uint64_t kTotal = kProducerCount * kMessagesPerProducer;
    using Channel = galay::mpsc::UnboundedChannel<Message>;

    Channel channel;
    std::vector<Channel::ProducerToken> tokens;
    if (useTokens) {
        tokens.reserve(kProducerCount);
        for (uint32_t producer = 0; producer < kProducerCount; ++producer) {
            auto token = channel.makeProducerToken();
            if (!token.valid()) {
                return false;
            }
            tokens.push_back(std::move(token));
        }
    }

    galay::benchmark::CompletionLatch ready(kProducerCount + 1);
    galay::benchmark::StartGate start;
    std::atomic<bool> failed{false};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (uint32_t producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            ready.arrive();
            start.wait();
            for (uint64_t sequence = 0; sequence < kMessagesPerProducer; ++sequence) {
                Message message{.sequence = sequence,
                                .producer = producer,
                                .sentinel = false};
                const bool sent = useTokens
                    ? channel.send(tokens[producer], std::move(message))
                    : channel.send(std::move(message));
                if (!sent) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }

    std::thread consumer([&]() {
        ready.arrive();
        start.wait();
        std::vector<uint64_t> expected(kProducerCount, 0);
        uint64_t received = 0;
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (received < kTotal && !failed.load(std::memory_order_acquire)) {
            auto value = channel.tryRecv();
            if (!value.has_value()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
                continue;
            }
            if (value->producer >= kProducerCount || value->sentinel ||
                value->sequence != expected[value->producer]) {
                failed.store(true, std::memory_order_release);
                return;
            }
            ++expected[value->producer];
            ++received;
        }
        for (uint64_t count : expected) {
            if (count != kMessagesPerProducer) {
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    });

    if (!ready.waitFor(2s)) {
        failed.store(true, std::memory_order_release);
    }
    start.open();
    for (std::thread& producer : producers) {
        producer.join();
    }
    consumer.join();
    return !failed.load(std::memory_order_acquire) && channel.empty();
}

bool testFairConsumerRotation()
{
    using Channel = galay::mpsc::UnboundedChannel<Message>;
    Channel channel;
    auto sparse = channel.makeProducerToken();
    auto hot = channel.makeProducerToken();
    if (!sparse.valid() || !hot.valid()) {
        return false;
    }

    for (uint64_t sequence = 0; sequence < 4'096; ++sequence) {
        Message message{.sequence = sequence, .producer = 0, .sentinel = false};
        if (!channel.send(hot, std::move(message))) {
            return false;
        }
    }
    Message sentinel{.sequence = 0, .producer = 1, .sentinel = true};
    if (!channel.send(sparse, std::move(sentinel))) {
        return false;
    }

    size_t beforeSentinel = 0;
    while (beforeSentinel <= 256) {
        auto value = channel.tryRecv();
        if (!value.has_value()) {
            return false;
        }
        if (value->sentinel) {
            return beforeSentinel <= 256;
        }
        ++beforeSentinel;
    }
    return false;
}

bool testInactiveToActiveHandoff()
{
    constexpr uint64_t kIterations = 100'000;
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    Channel channel(Channel::DEFAULT_BATCH_SIZE, 0);
    auto token = channel.makeProducerToken();
    if (!token.valid()) {
        return false;
    }

    std::atomic<uint64_t> consumed{0};
    std::atomic<bool> failed{false};
    std::thread producer([&]() {
        for (uint64_t sequence = 1; sequence <= kIterations; ++sequence) {
            while (consumed.load(std::memory_order_acquire) != sequence - 1) {
                std::this_thread::yield();
            }
            uint64_t value = sequence;
            if (!channel.send(token, std::move(value))) {
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + 15s;
    for (uint64_t expected = 1;
         expected <= kIterations && !failed.load(std::memory_order_acquire);
         ++expected) {
        std::optional<uint64_t> value;
        while (!(value = channel.tryRecv()).has_value()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                failed.store(true, std::memory_order_release);
                break;
            }
            std::this_thread::yield();
        }
        if (!value.has_value() || *value != expected) {
            failed.store(true, std::memory_order_release);
            break;
        }
        consumed.store(expected, std::memory_order_release);
    }

    producer.join();
    return !failed.load(std::memory_order_acquire) &&
        consumed.load(std::memory_order_acquire) == kIterations &&
        channel.empty() &&
        galay::mpsc::UnboundedChannelTestAccess::allocatedBlockCount(channel) <= 2 &&
        galay::mpsc::UnboundedChannelTestAccess::recycledBlockCount(channel) >= 1;
}

bool testTokenReleaseReacquireHandoffWhileConsuming()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    constexpr uint64_t kMessages = 50'000;
    constexpr uint64_t kProducerCount = 2;
    constexpr uint64_t kMaxInFlight = 64;
    constexpr uint64_t kStop = kMessages + 1;

    Channel channel;
    galay::benchmark::CompletionLatch ready(kProducerCount + 1);
    galay::benchmark::StartGate start;
    std::atomic<uint64_t> turn{0};
    std::atomic<uint64_t> consumed{0};
    std::atomic<bool> failed{false};
    const auto fail = [&]() {
        failed.store(true, std::memory_order_release);
        turn.store(kStop, std::memory_order_release);
        consumed.store(kStop, std::memory_order_release);
        turn.notify_all();
        consumed.notify_all();
    };

    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (uint64_t producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            ready.arrive();
            start.wait();
            for (uint64_t sequence = producer;
                 sequence < kMessages;
                 sequence += kProducerCount) {
                uint64_t observed = turn.load(std::memory_order_acquire);
                while (observed != sequence) {
                    if (observed == kStop ||
                        failed.load(std::memory_order_acquire)) {
                        return;
                    }
                    turn.wait(observed, std::memory_order_acquire);
                    observed = turn.load(std::memory_order_acquire);
                }

                {
                    auto token = channel.makeProducerToken();
                    if (!token.valid()) {
                        fail();
                        return;
                    }
                    uint64_t value = sequence;
                    if (!channel.send(token, std::move(value))) {
                        fail();
                        return;
                    }
                }

                if (sequence >= kMaxInFlight) {
                    const uint64_t requiredConsumed =
                        sequence - kMaxInFlight + 1;
                    uint64_t observed =
                        consumed.load(std::memory_order_acquire);
                    while (observed < requiredConsumed) {
                        if (observed == kStop ||
                            failed.load(std::memory_order_acquire)) {
                            return;
                        }
                        consumed.wait(observed, std::memory_order_acquire);
                        observed = consumed.load(std::memory_order_acquire);
                    }
                }
                uint64_t expectedTurn = sequence;
                if (!turn.compare_exchange_strong(expectedTurn,
                                                  sequence + 1,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                    return;
                }
                turn.notify_all();
            }
        });
    }

    std::thread consumer([&]() {
        ready.arrive();
        start.wait();
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        for (uint64_t expected = 0; expected < kMessages; ++expected) {
            std::optional<uint64_t> received;
            while (!(received = channel.tryRecv()).has_value()) {
                if (failed.load(std::memory_order_acquire) ||
                    std::chrono::steady_clock::now() >= deadline) {
                    fail();
                    return;
                }
                std::this_thread::yield();
            }
            if (*received != expected) {
                fail();
                return;
            }
            consumed.store(expected + 1, std::memory_order_release);
            consumed.notify_all();
        }
    });

    if (!ready.waitFor(2s)) {
        fail();
    }
    start.open();
    for (std::thread& producer : producers) {
        producer.join();
    }
    consumer.join();

    return !failed.load(std::memory_order_acquire) && channel.empty() &&
        galay::mpsc::UnboundedChannelTestAccess::allocatedStreamCount(channel) ==
            1;
}

bool testTokenAndTlsLifetime()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    Channel channel;
    {
        auto token = channel.makeProducerToken();
        if (!token.valid() || !channel.send(token, 1)) {
            return false;
        }
        auto moved = std::move(token);
        if (token.valid() || !moved.valid() || !channel.send(moved, 2)) {
            return false;
        }
        Channel other;
        if (other.send(moved, 3)) {
            return false;
        }
    }

    auto first = channel.tryRecv();
    auto second = channel.tryRecv();
    if (!first.has_value() || !second.has_value() || *first != 1 || *second != 2) {
        return false;
    }

    alignas(Channel) std::byte storage[sizeof(Channel)];
    Channel* original = std::construct_at(reinterpret_cast<Channel*>(storage));
    if (!original->send(11)) {
        std::destroy_at(original);
        return false;
    }
    auto originalValue = original->tryRecv();
    const bool originalOk = originalValue.has_value() && *originalValue == 11;
    std::destroy_at(original);
    if (!originalOk) {
        return false;
    }

    Channel* replacement = std::construct_at(reinterpret_cast<Channel*>(storage));
    const bool sent = replacement->send(12);
    auto replacementValue = replacement->tryRecv();
    const bool replacementOk = sent && replacementValue.has_value() &&
        *replacementValue == 12 && replacement->empty();
    std::destroy_at(replacement);
    return replacementOk;
}

bool testExplicitProducerStreamsAreReused()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    constexpr uint64_t kIterations = 4'096;
    Channel channel;
    for (uint64_t value = 0; value < kIterations; ++value) {
        auto token = channel.makeProducerToken();
        if (!token.valid() || !channel.send(token, value)) {
            return false;
        }
    }
    if (galay::mpsc::UnboundedChannelTestAccess::allocatedStreamCount(channel) != 1) {
        return false;
    }
    for (uint64_t value = 0; value < kIterations; ++value) {
        auto received = channel.tryRecv();
        if (!received.has_value() || *received != value) {
            return false;
        }
    }
    return !channel.tryRecv().has_value();
}

bool testTokenMoveAssignmentReleasesPreviousStream()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    Channel channel;
    auto first = channel.makeProducerToken();
    auto second = channel.makeProducerToken();
    if (!first.valid() || !second.valid()) {
        return false;
    }

    first = std::move(second);
    auto third = channel.makeProducerToken();
    return first.valid() && !second.valid() && third.valid() &&
        galay::mpsc::UnboundedChannelTestAccess::allocatedStreamCount(channel) <= 2;
}

bool testTlsProducerStreamsAreReusedAfterThreadExit()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    constexpr uint64_t kIterations = 512;
    Channel channel;
    for (uint64_t value = 0; value < kIterations; ++value) {
        std::atomic<bool> sent{false};
        std::thread producer([&]() {
            sent.store(channel.send(value), std::memory_order_release);
        });
        producer.join();
        if (!sent.load(std::memory_order_acquire)) {
            return false;
        }
    }
    if (galay::mpsc::UnboundedChannelTestAccess::allocatedStreamCount(channel) != 1) {
        return false;
    }
    for (uint64_t value = 0; value < kIterations; ++value) {
        auto received = channel.tryRecv();
        if (!received.has_value() || *received != value) {
            return false;
        }
    }
    return !channel.tryRecv().has_value();
}

bool testTlsCacheCanOutliveChannel()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    alignas(Channel) std::byte storage[sizeof(Channel)];
    Channel* channel = std::construct_at(reinterpret_cast<Channel*>(storage));
    std::atomic<bool> ready{false};
    std::atomic<bool> release{false};
    std::atomic<bool> sent{false};
    std::thread producer([&]() {
        sent.store(channel->send(77), std::memory_order_release);
        ready.store(true, std::memory_order_release);
        ready.notify_all();
        release.wait(false, std::memory_order_acquire);
    });

    ready.wait(false, std::memory_order_acquire);
    auto received = channel->tryRecv();
    const bool passed = sent.load(std::memory_order_acquire) &&
        received.has_value() && *received == 77;
    std::destroy_at(channel);
    release.store(true, std::memory_order_release);
    release.notify_all();
    producer.join();
    return passed;
}

bool testDetachedTokenReportsInvalid()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    alignas(Channel) std::byte storage[sizeof(Channel)];
    Channel* channel = std::construct_at(reinterpret_cast<Channel*>(storage));
    auto token = channel->makeProducerToken();
    const bool validBeforeDestroy = token.valid();
    std::destroy_at(channel);
    const bool invalidAfterDestroy = !token.valid();

    Channel* replacement = std::construct_at(reinterpret_cast<Channel*>(storage));
    uint64_t value = 9;
    const bool staleTokenRejected =
        !replacement->send(token, std::move(value));
    std::destroy_at(replacement);
    return validBeforeDestroy && invalidAfterDestroy && staleTokenRejected;
}

bool testConcurrentProducerClaimsRespectHighWater()
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    constexpr size_t kProducerCount = 8;
    constexpr size_t kRounds = 512;
    constexpr size_t kTotal = kProducerCount * kRounds;

    Channel channel;
    std::barrier acquired(kProducerCount);
    std::barrier sent(kProducerCount);
    std::barrier released(kProducerCount);
    std::atomic<bool> failed{false};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (size_t producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            for (size_t round = 0; round < kRounds; ++round) {
                {
                    auto token = channel.makeProducerToken();
                    const bool valid = token.valid();
                    if (!valid) {
                        failed.store(true, std::memory_order_release);
                    }
                    acquired.arrive_and_wait();
                    if (valid) {
                        uint64_t value = producer * kRounds + round;
                        if (!channel.send(token, std::move(value))) {
                            failed.store(true, std::memory_order_release);
                        }
                    }
                    sent.arrive_and_wait();
                }
                released.arrive_and_wait();
            }
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    if (failed.load(std::memory_order_acquire) ||
        galay::mpsc::UnboundedChannelTestAccess::allocatedStreamCount(channel) !=
            kProducerCount) {
        return false;
    }

    std::vector<bool> seen(kTotal, false);
    for (size_t count = 0; count < kTotal; ++count) {
        auto value = channel.tryRecv();
        if (!value.has_value() || *value >= kTotal || seen[*value]) {
            return false;
        }
        seen[*value] = true;
    }
    return !channel.tryRecv().has_value();
}

} // namespace

int main()
{
    galay::test::TestResultWriter writer("t155_mpsc_unbounded_streams");
    writer.addTest();

    bool passed = true;
    const auto check = [&passed](bool result, const char* name) {
        if (!result) {
            std::cerr << name << " failed\n";
            passed = false;
        }
    };
    check(testMoveOnlyAcrossBlocks(), "move-only across blocks");
    check(testNonTrivialValueLifetimeAcrossRecycleAndDestruction(),
          "non-trivial value lifetime across recycle and destruction");
    check(runPerProducerFifo(true), "token per-producer FIFO");
    check(runPerProducerFifo(false), "TLS per-producer FIFO");
    check(testFairConsumerRotation(), "fair consumer rotation");
    check(testInactiveToActiveHandoff(), "inactive-to-active handoff");
    check(testTokenReleaseReacquireHandoffWhileConsuming(),
          "token release/reacquire handoff while consuming");
    check(testTokenAndTlsLifetime(), "token and TLS lifetime");
    check(testExplicitProducerStreamsAreReused(),
          "explicit producer streams are reused");
    check(testTokenMoveAssignmentReleasesPreviousStream(),
          "token move assignment releases previous stream");
    check(testTlsProducerStreamsAreReusedAfterThreadExit(),
          "TLS producer streams are reused after thread exit");
    check(testTlsCacheCanOutliveChannel(), "TLS cache can outlive channel");
    check(testDetachedTokenReportsInvalid(),
          "detached token reports invalid");
    check(testConcurrentProducerClaimsRespectHighWater(),
          "concurrent producer claims respect high water");
    if (!passed) {
        std::cerr << "MPSC unbounded stream test failed\n";
        writer.addFailed();
        writer.writeResult();
        return 1;
    }

    writer.addPassed();
    writer.writeResult();
    std::cout << "t155_mpsc_unbounded_streams PASS\n";
    return 0;
}
