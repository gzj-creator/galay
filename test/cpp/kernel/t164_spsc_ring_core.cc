/**
 * @file t164_spsc_ring_core.cc
 * @brief 验证 SPSC ring 数据面的满容量、游标回绕、移动语义和批量 FIFO。
 */

#include "result_writer.h"

#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using galay::spsc::Ring;
using galay::spsc::RingError;
using galay::spsc::RingValue;
using galay::spsc::StaticRing;
using galay::spsc::BoundedValue;
using namespace std::chrono_literals;

struct ThrowingMove
{
    ThrowingMove() = default;
    ThrowingMove(ThrowingMove&&) noexcept(false) {}
    ThrowingMove& operator=(ThrowingMove&&) noexcept(false)
    {
        return *this;
    }
};

struct ThrowingCopy
{
    ThrowingCopy() = default;
    ThrowingCopy(const ThrowingCopy&) noexcept(false) {}
    ThrowingCopy& operator=(const ThrowingCopy&) noexcept(false)
    {
        return *this;
    }
    ThrowingCopy(ThrowingCopy&&) noexcept = default;
    ThrowingCopy& operator=(ThrowingCopy&&) noexcept = default;
};

struct LifetimeValue
{
    inline static int alive = 0;

    LifetimeValue() noexcept
    {
        ++alive;
    }

    explicit LifetimeValue(int input) noexcept : value(input)
    {
        ++alive;
    }

    LifetimeValue(const LifetimeValue&) = delete;
    LifetimeValue& operator=(const LifetimeValue&) = delete;

    LifetimeValue(LifetimeValue&& other) noexcept
        : value(std::exchange(other.value, -1))
    {
        ++alive;
    }

    LifetimeValue& operator=(LifetimeValue&& other) noexcept
    {
        value = std::exchange(other.value, -1);
        return *this;
    }

    ~LifetimeValue() noexcept
    {
        --alive;
    }

    int value = -1;
};

template <typename T>
concept SupportsConstBoundedSend = requires(
    galay::spsc::BoundedChannel<T>& channel, const T& value) {
    { channel.trySend(value) } -> std::same_as<bool>;
};

static_assert(RingValue<std::unique_ptr<int>>);
static_assert(!RingValue<ThrowingMove>);
static_assert(!BoundedValue<ThrowingMove>);
static_assert(BoundedValue<ThrowingCopy>);
static_assert(!SupportsConstBoundedSend<ThrowingCopy>);
static_assert(std::is_default_constructible_v<StaticRing<int, 4>>);
static_assert(!std::is_constructible_v<StaticRing<int, 4>, size_t>);

bool testFullCapacityAndMoveOnlyValue()
{
    Ring<std::unique_ptr<int>> ring(4);
    if (ring.error() != RingError::kNone || ring.capacity() != 4) {
        return false;
    }

    for (int value = 0; value < 4; ++value) {
        auto item = std::make_unique<int>(value);
        if (!ring.tryWrite(std::move(item)) || item) {
            return false;
        }
    }

    auto rejected = std::make_unique<int>(99);
    if (ring.tryWrite(std::move(rejected)) || !rejected || *rejected != 99) {
        return false;
    }

    for (int expected = 0; expected < 4; ++expected) {
        auto item = ring.tryRead();
        if (!item.has_value() || !*item || **item != expected) {
            return false;
        }
    }
    return !ring.tryRead().has_value();
}

bool testCursorWraparound()
{
    Ring<uint16_t, uint8_t> ring(4);
    if (ring.error() != RingError::kNone) {
        return false;
    }
    constexpr uint16_t kCycles = 80;
    for (uint16_t cycle = 0; cycle < kCycles; ++cycle) {
        for (uint16_t offset = 0; offset < 4; ++offset) {
            uint16_t value = static_cast<uint16_t>(cycle * 4 + offset);
            if (!ring.tryWrite(std::move(value))) {
                return false;
            }
        }
        for (uint16_t offset = 0; offset < 4; ++offset) {
            const uint16_t expected = static_cast<uint16_t>(cycle * 4 + offset);
            auto value = ring.tryRead();
            if (!value.has_value() || *value != expected) {
                return false;
            }
        }
    }
    return !ring.tryRead().has_value();
}

bool testCallerOwnedBatchFifo()
{
    Ring<int> ring(8);
    if (ring.error() != RingError::kNone) {
        return false;
    }
    std::array<int, 6> input{10, 11, 12, 13, 14, 15};
    if (ring.tryWriteBatch(std::span<int>(input)) != input.size()) {
        return false;
    }

    std::array<int, 4> first{};
    std::array<int, 4> second{};
    const size_t firstCount = ring.tryReadBatch(std::span<int>(first));
    const size_t secondCount = ring.tryReadBatch(std::span<int>(second));
    const size_t emptyCount = ring.tryReadBatch(std::span<int>(second));

    return firstCount == 4 && secondCount == 2 && emptyCount == 0 &&
        first == std::array<int, 4>{10, 11, 12, 13} && second[0] == 14 &&
        second[1] == 15;
}

bool testTrivialBatchWrapAndPartial()
{
    Ring<uint64_t> ring(8);
    if (ring.error() != RingError::kNone) {
        return false;
    }

    std::array<uint64_t, 6> firstInput{10, 11, 12, 13, 14, 15};
    if (ring.tryWriteBatch(std::span<uint64_t>(firstInput)) != firstInput.size()) {
        return false;
    }

    std::array<uint64_t, 5> prefix{};
    if (ring.tryReadBatch(std::span<uint64_t>(prefix)) != prefix.size() ||
        prefix != std::array<uint64_t, 5>{10, 11, 12, 13, 14}) {
        return false;
    }

    std::array<uint64_t, 7> wrappedInput{100, 101, 102, 103, 104, 105, 106};
    if (ring.tryWriteBatch(std::span<uint64_t>(wrappedInput)) !=
        wrappedInput.size()) {
        return false;
    }

    std::array<uint64_t, 8> wrappedOutput{};
    if (ring.tryReadBatch(std::span<uint64_t>(wrappedOutput)) !=
            wrappedOutput.size() ||
        wrappedOutput !=
            std::array<uint64_t, 8>{15, 100, 101, 102, 103, 104, 105, 106}) {
        return false;
    }

    std::array<uint64_t, 10> oversizedInput{200, 201, 202, 203, 204,
                                             205, 206, 207, 208, 209};
    if (ring.tryWriteBatch(std::span<uint64_t>(oversizedInput)) !=
        ring.capacity()) {
        return false;
    }
    std::array<uint64_t, 8> partialOutput{};
    return ring.tryReadBatch(std::span<uint64_t>(partialOutput)) ==
            partialOutput.size() &&
        partialOutput ==
            std::array<uint64_t, 8>{200, 201, 202, 203, 204, 205, 206, 207};
}

bool testNonTrivialBatchLifetime()
{
    if (LifetimeValue::alive != 0) {
        return false;
    }

    bool passed = false;
    {
        Ring<LifetimeValue> ring(4);
        std::array<LifetimeValue, 4> input{};
        std::array<LifetimeValue, 4> output{};
        for (size_t index = 0; index < input.size(); ++index) {
            input[index].value = static_cast<int>(index + 1);
        }

        const size_t sent = ring.tryWriteBatch(std::span<LifetimeValue>(input));
        const size_t received =
            ring.tryReadBatch(std::span<LifetimeValue>(output));
        passed = ring.error() == RingError::kNone && sent == input.size() &&
            received == output.size() && LifetimeValue::alive == 8 &&
            output[0].value == 1 && output[1].value == 2 &&
            output[2].value == 3 && output[3].value == 4;
    }
    return passed && LifetimeValue::alive == 0;
}

bool testSplitBatchEndpoints()
{
    Ring<uint64_t> ring(8);
    if (ring.error() != RingError::kNone) {
        return false;
    }
    auto endpoints = ring.split();
    std::array<uint64_t, 6> input{31, 32, 33, 34, 35, 36};
    std::array<uint64_t, 6> output{};
    return endpoints.producer.tryWriteBatch(std::span<uint64_t>(input)) ==
            input.size() &&
        endpoints.consumer.tryReadBatch(std::span<uint64_t>(output)) ==
            output.size() &&
        output == input;
}

bool testCallerOwnedSingleReceive()
{
    Ring<int> ring(2);
    int input = 73;
    int output = -1;
    return ring.error() == RingError::kNone &&
        ring.tryWrite(std::move(input)) && ring.tryRead(output) && output == 73 &&
        !ring.tryRead(output) && output == 73;
}

bool testCapacityValidation()
{
    Ring<int, uint8_t> ring(129);
    return ring.error() == RingError::kCapacityTooLarge && ring.capacity() == 0;
}

bool testStaticAliasFullCapacity()
{
    StaticRing<int, 4, uint8_t> ring;
    for (int value = 0; value < 4; ++value) {
        int item = value;
        if (!ring.tryWrite(std::move(item))) {
            return false;
        }
    }
    int rejected = 99;
    if (ring.tryWrite(std::move(rejected)) || rejected != 99) {
        return false;
    }
    for (int expected = 0; expected < 4; ++expected) {
        int output = -1;
        if (!ring.tryRead(output) || output != expected) {
            return false;
        }
    }
    return ring.capacity() == 4 && !ring.tryRead().has_value();
}

bool testCrossThreadFifo()
{
    constexpr uint32_t kMessages = 250'000;
    StaticRing<uint32_t, 256> ring;
    if (ring.error() != RingError::kNone) {
        return false;
    }
    std::atomic<bool> failed{false};
    std::atomic<uint32_t> received{0};

    std::thread producer([&]() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (uint32_t sequence = 0; sequence < kMessages; ++sequence) {
            uint32_t value = sequence;
            while (!ring.tryWrite(std::move(value))) {
                if (failed.load(std::memory_order_acquire) ||
                    std::chrono::steady_clock::now() >= deadline) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        uint32_t expected = 0;
        while (expected < kMessages) {
            auto value = ring.tryRead();
            if (!value.has_value()) {
                if (failed.load(std::memory_order_acquire) ||
                    std::chrono::steady_clock::now() >= deadline) {
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
        }
        received.store(expected, std::memory_order_release);
    });

    producer.join();
    consumer.join();
    return !failed.load(std::memory_order_acquire) &&
        received.load(std::memory_order_acquire) == kMessages;
}

} // namespace

int main()
{
    const std::array results{
        std::pair{"full_capacity_move_only", testFullCapacityAndMoveOnlyValue()},
        std::pair{"cursor_wraparound", testCursorWraparound()},
        std::pair{"caller_owned_batch_fifo", testCallerOwnedBatchFifo()},
        std::pair{"trivial_batch_wrap_partial", testTrivialBatchWrapAndPartial()},
        std::pair{"non_trivial_batch_lifetime", testNonTrivialBatchLifetime()},
        std::pair{"split_batch_endpoints", testSplitBatchEndpoints()},
        std::pair{"caller_owned_single_receive", testCallerOwnedSingleReceive()},
        std::pair{"capacity_validation", testCapacityValidation()},
        std::pair{"static_alias_full_capacity", testStaticAliasFullCapacity()},
        std::pair{"cross_thread_fifo", testCrossThreadFifo()},
    };

    galay::test::TestResultWriter writer("t164_spsc_ring_core");
    bool passed = true;
    for (const auto& [name, result] : results) {
        writer.addTest();
        if (result) {
            writer.addPassed();
        } else {
            writer.addFailed();
            passed = false;
        }
        std::cout << name << '=' << (result ? "PASS" : "FAIL") << '\n';
    }
    writer.writeResult();
    return passed ? 0 : 1;
}
