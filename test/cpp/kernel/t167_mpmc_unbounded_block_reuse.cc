/**
 * @file t167_mpmc_unbounded_block_reuse.cc
 * @brief 验证 MPMC unbounded 显式/默认 token 的跨 block 两波复用。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/unbounded_channel.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace galay::mpmc {

struct UnboundedChannelTestAccess
{
    template <UnboundedValue T>
    static size_t blockCount(const UnboundedChannel<T>& channel) noexcept
    {
        size_t count = 0;
        auto* block = channel.m_allBlocks;
        while (block != nullptr) {
            ++count;
            block = block->allNext;
        }
        return count;
    }
};

} // namespace galay::mpmc

namespace {

using Channel = galay::mpmc::UnboundedChannel<int>;

struct ThrowingDefaultValue {
    ThrowingDefaultValue() noexcept(false) {}
    ThrowingDefaultValue(ThrowingDefaultValue&&) noexcept = default;
    ThrowingDefaultValue& operator=(ThrowingDefaultValue&&) noexcept = default;
};

struct ThrowingMoveValue {
    ThrowingMoveValue() noexcept = default;
    ThrowingMoveValue(ThrowingMoveValue&&) noexcept(false) {}
    ThrowingMoveValue& operator=(ThrowingMoveValue&&) noexcept = default;
};

struct ThrowingMoveAssignValue {
    ThrowingMoveAssignValue() noexcept = default;
    ThrowingMoveAssignValue(ThrowingMoveAssignValue&&) noexcept = default;
    ThrowingMoveAssignValue& operator=(ThrowingMoveAssignValue&&) noexcept(false)
    {
        return *this;
    }
};

struct ThrowingCopyValue {
    ThrowingCopyValue() noexcept = default;
    ThrowingCopyValue(const ThrowingCopyValue&) noexcept(false) {}
    ThrowingCopyValue(ThrowingCopyValue&&) noexcept = default;
    ThrowingCopyValue& operator=(ThrowingCopyValue&&) noexcept = default;
};

template <typename T>
concept HasUnboundedCopySend = requires(
    galay::mpmc::UnboundedChannel<T>& channel,
    typename galay::mpmc::UnboundedChannel<T>::ProducerToken& token,
    const T& value,
    const std::vector<T>& values) {
    channel.send(value);
    channel.send(token, value);
    channel.sendBatch(values);
    channel.sendBatch(token, values);
};

static_assert(galay::mpmc::UnboundedValue<int>);
static_assert(std::default_initializable<ThrowingDefaultValue>);
static_assert(std::movable<ThrowingDefaultValue>);
static_assert(std::movable<ThrowingMoveValue>);
static_assert(std::movable<ThrowingMoveAssignValue>);
static_assert(std::copy_constructible<ThrowingCopyValue>);
static_assert(!galay::mpmc::UnboundedValue<ThrowingDefaultValue>);
static_assert(!galay::mpmc::UnboundedValue<ThrowingMoveValue>);
static_assert(!galay::mpmc::UnboundedValue<ThrowingMoveAssignValue>);
static_assert(galay::mpmc::UnboundedValue<ThrowingCopyValue>);
static_assert(HasUnboundedCopySend<int>);
static_assert(!HasUnboundedCopySend<ThrowingCopyValue>);

bool waitFor(const std::atomic<int>& value, int expected)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_acquire) == expected) {
            return true;
        }
        std::this_thread::yield();
    }
    return value.load(std::memory_order_acquire) == expected;
}

struct TrackedReadyValue
{
    inline static std::atomic<int> live{0};
    inline static std::atomic<int> readyDestroyed{0};

    int value = -1;

    TrackedReadyValue() noexcept
    {
        live.fetch_add(1, std::memory_order_relaxed);
    }

    explicit TrackedReadyValue(int input) noexcept : value(input)
    {
        live.fetch_add(1, std::memory_order_relaxed);
    }

    TrackedReadyValue(const TrackedReadyValue&) = delete;
    TrackedReadyValue& operator=(const TrackedReadyValue&) = delete;

    TrackedReadyValue(TrackedReadyValue&& other) noexcept
        : value(std::exchange(other.value, -1))
    {
        live.fetch_add(1, std::memory_order_relaxed);
    }

    TrackedReadyValue& operator=(TrackedReadyValue&& other) noexcept
    {
        value = std::exchange(other.value, -1);
        return *this;
    }

    ~TrackedReadyValue() noexcept
    {
        if (value >= 0) {
            readyDestroyed.fetch_add(1, std::memory_order_relaxed);
        }
        live.fetch_sub(1, std::memory_order_relaxed);
    }
};

static_assert(galay::mpmc::UnboundedValue<TrackedReadyValue>);

bool checkReadyValueDestruction()
{
    constexpr int kReadyValues = 5'000;
    TrackedReadyValue::live.store(0, std::memory_order_relaxed);
    TrackedReadyValue::readyDestroyed.store(0, std::memory_order_relaxed);

    bool staged = true;
    {
        galay::mpmc::UnboundedChannel<TrackedReadyValue> channel;
        for (int value = 0; value < kReadyValues; ++value) {
            TrackedReadyValue input(value);
            if (!channel.send(std::move(input))) {
                staged = false;
                break;
            }
        }
        staged = staged && channel.size() == static_cast<size_t>(kReadyValues) &&
            TrackedReadyValue::live.load(std::memory_order_relaxed) ==
                kReadyValues &&
            TrackedReadyValue::readyDestroyed.load(std::memory_order_relaxed) == 0;
    }

    return staged &&
        TrackedReadyValue::live.load(std::memory_order_relaxed) == 0 &&
        TrackedReadyValue::readyDestroyed.load(std::memory_order_relaxed) ==
            kReadyValues;
}

template <bool UseToken>
bool checkBlockReuse()
{
    constexpr int kProducerCount = 4;
    constexpr int kConsumerCount = 4;
    constexpr int kPhaseCount = 2;
    constexpr int kMessagesPerProducer = 3'000;
    constexpr int kMessagesPerPhase = kProducerCount * kMessagesPerProducer;
    constexpr int kMessageCount = kPhaseCount * kMessagesPerPhase;

    Channel channel;
    auto seen = std::make_unique<std::atomic_uint8_t[]>(kMessageCount);
    for (int index = 0; index < kMessageCount; ++index) {
        seen[static_cast<size_t>(index)].store(0, std::memory_order_relaxed);
    }
    std::array<std::atomic<int>, kPhaseCount> received{};
    std::atomic<int> producerReady{0};
    std::atomic<int> consumerReady{0};
    std::atomic<int> producersDone{0};
    std::atomic<int> consumersDone{0};
    std::atomic<int> producePhase{-1};
    std::atomic<int> consumePhase{-1};
    std::atomic<bool> failed{false};
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            std::optional<typename Channel::ProducerToken> token;
            if constexpr (UseToken) {
                token.emplace(channel.makeProducerToken());
                if (!token->valid()) {
                    failed.store(true, std::memory_order_release);
                }
            }
            producerReady.fetch_add(1, std::memory_order_release);
            for (int phase = 0; phase < kPhaseCount; ++phase) {
                while (!failed.load(std::memory_order_acquire) &&
                       producePhase.load(std::memory_order_acquire) < phase) {
                    std::this_thread::yield();
                }
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
                const int first = phase * kMessagesPerPhase +
                    producer * kMessagesPerProducer;
                for (int offset = 0; offset < kMessagesPerProducer; ++offset) {
                    const bool sent = UseToken
                        ? channel.send(*token, first + offset)
                        : channel.send(first + offset);
                    if (!sent) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                }
                producersDone.fetch_add(1, std::memory_order_release);
            }
        });
    }

    for (int consumer = 0; consumer < kConsumerCount; ++consumer) {
        consumers.emplace_back([&]() {
            std::optional<typename Channel::ConsumerToken> token;
            if constexpr (UseToken) {
                token.emplace(channel.makeConsumerToken());
                if (!token->valid()) {
                    failed.store(true, std::memory_order_release);
                }
            }
            consumerReady.fetch_add(1, std::memory_order_release);
            for (int phase = 0; phase < kPhaseCount; ++phase) {
                while (!failed.load(std::memory_order_acquire) &&
                       consumePhase.load(std::memory_order_acquire) < phase) {
                    std::this_thread::yield();
                }
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
                while (!failed.load(std::memory_order_acquire) &&
                       received[static_cast<size_t>(phase)].load(
                           std::memory_order_acquire) < kMessagesPerPhase) {
                    std::optional<int> value = UseToken
                        ? channel.tryRecv(*token)
                        : channel.tryRecv();
                    if (!value.has_value()) {
                        std::this_thread::yield();
                        continue;
                    }
                    const int first = phase * kMessagesPerPhase;
                    const int last = first + kMessagesPerPhase;
                    if (*value < first || *value >= last ||
                        seen[static_cast<size_t>(*value)].fetch_add(
                            1, std::memory_order_relaxed) != 0) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    received[static_cast<size_t>(phase)].fetch_add(
                        1, std::memory_order_release);
                }
                consumersDone.fetch_add(1, std::memory_order_release);
            }
        });
    }

    bool ok = waitFor(producerReady, kProducerCount) &&
        waitFor(consumerReady, kConsumerCount);
    for (int phase = 0; ok && phase < kPhaseCount; ++phase) {
        producePhase.store(phase, std::memory_order_release);
        ok = waitFor(producersDone, (phase + 1) * kProducerCount) &&
            channel.size() == static_cast<size_t>(kMessagesPerPhase) &&
            !channel.empty();
        if (!ok) {
            break;
        }
        consumePhase.store(phase, std::memory_order_release);
        ok = waitFor(received[static_cast<size_t>(phase)], kMessagesPerPhase) &&
            waitFor(consumersDone, (phase + 1) * kConsumerCount) &&
            channel.empty() && channel.size() == 0;
    }

    if (!ok) {
        failed.store(true, std::memory_order_release);
        producePhase.store(kPhaseCount, std::memory_order_release);
        consumePhase.store(kPhaseCount, std::memory_order_release);
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    for (std::thread& consumer : consumers) {
        consumer.join();
    }
    if (!ok || failed.load(std::memory_order_acquire)) {
        return false;
    }
    for (int index = 0; index < kMessageCount; ++index) {
        if (seen[static_cast<size_t>(index)].load(std::memory_order_relaxed) != 1) {
            return false;
        }
    }
    channel.close();
    return channel.isClosed() && channel.empty() && channel.size() == 0;
}

template <bool UseToken>
bool checkBatchReuse()
{
    constexpr int kPhaseCount = 2;
    constexpr int kMessagesPerPhase = 12'345;
    Channel channel;
    std::optional<typename Channel::ProducerToken> producerToken;
    std::optional<typename Channel::ConsumerToken> consumerToken;
    if constexpr (UseToken) {
        producerToken.emplace(channel.makeProducerToken());
        consumerToken.emplace(channel.makeConsumerToken());
        if (!producerToken->valid() || !consumerToken->valid()) {
            return false;
        }
    }

    size_t firstPhaseBlocks = 0;
    for (int phase = 0; phase < kPhaseCount; ++phase) {
        const int first = phase * kMessagesPerPhase;
        std::vector<int> values(static_cast<size_t>(kMessagesPerPhase));
        std::iota(values.begin(), values.end(), first);
        const bool sent = UseToken
            ? channel.sendBatch(*producerToken, std::move(values))
            : channel.sendBatch(std::move(values));
        if (!sent || channel.size() != static_cast<size_t>(kMessagesPerPhase)) {
            return false;
        }
        const size_t currentBlocks =
            galay::mpmc::UnboundedChannelTestAccess::blockCount(channel);
        if (phase == 0) {
            firstPhaseBlocks = currentBlocks;
        } else if (currentBlocks != firstPhaseBlocks) {
            return false;
        }

        int expected = first;
        while (expected < first + kMessagesPerPhase) {
            auto received = UseToken
                ? channel.tryRecvBatch(*consumerToken, 5'000)
                : channel.tryRecvBatch(5'000);
            if (!received.has_value() || received->empty()) {
                return false;
            }
            for (const int value : *received) {
                if (value != expected) {
                    return false;
                }
                ++expected;
            }
        }
        if (!channel.empty() || channel.size() != 0) {
            return false;
        }
    }

    channel.close();
    return channel.isClosed() && channel.empty();
}

} // namespace

int main()
{
    if (!checkReadyValueDestruction() || !checkBlockReuse<false>() ||
        !checkBlockReuse<true>() || !checkBatchReuse<false>() ||
        !checkBatchReuse<true>()) {
        std::cerr << "MPMC unbounded block reuse test failed\n";
        return 1;
    }
    std::cout << "T167-MpmcUnboundedBlockReuse PASS\n";
    return 0;
}
