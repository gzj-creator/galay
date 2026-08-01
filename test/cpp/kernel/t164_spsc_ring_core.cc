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
        if (!ring.trySend(std::move(item)) || item) {
            return false;
        }
    }

    auto rejected = std::make_unique<int>(99);
    if (ring.trySend(std::move(rejected)) || !rejected || *rejected != 99) {
        return false;
    }

    for (int expected = 0; expected < 4; ++expected) {
        auto item = ring.tryRecv();
        if (!item.has_value() || !*item || **item != expected) {
            return false;
        }
    }
    return !ring.tryRecv().has_value();
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
            if (!ring.trySend(std::move(value))) {
                return false;
            }
        }
        for (uint16_t offset = 0; offset < 4; ++offset) {
            const uint16_t expected = static_cast<uint16_t>(cycle * 4 + offset);
            auto value = ring.tryRecv();
            if (!value.has_value() || *value != expected) {
                return false;
            }
        }
    }
    return !ring.tryRecv().has_value();
}

bool testCallerOwnedBatchFifo()
{
    Ring<int> ring(8);
    if (ring.error() != RingError::kNone) {
        return false;
    }
    std::array<int, 6> input{10, 11, 12, 13, 14, 15};
    if (ring.trySendBatch(std::span<int>(input)) != input.size()) {
        return false;
    }

    std::array<int, 4> first{};
    std::array<int, 4> second{};
    const size_t firstCount = ring.tryRecvBatch(std::span<int>(first));
    const size_t secondCount = ring.tryRecvBatch(std::span<int>(second));
    const size_t emptyCount = ring.tryRecvBatch(std::span<int>(second));

    return firstCount == 4 && secondCount == 2 && emptyCount == 0 &&
        first == std::array<int, 4>{10, 11, 12, 13} && second[0] == 14 &&
        second[1] == 15;
}

bool testCallerOwnedSingleReceive()
{
    Ring<int> ring(2);
    int input = 73;
    int output = -1;
    return ring.error() == RingError::kNone &&
        ring.trySend(std::move(input)) && ring.tryRecv(output) && output == 73 &&
        !ring.tryRecv(output) && output == 73;
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
        if (!ring.trySend(std::move(item))) {
            return false;
        }
    }
    int rejected = 99;
    if (ring.trySend(std::move(rejected)) || rejected != 99) {
        return false;
    }
    for (int expected = 0; expected < 4; ++expected) {
        int output = -1;
        if (!ring.tryRecv(output) || output != expected) {
            return false;
        }
    }
    return ring.capacity() == 4 && !ring.tryRecv().has_value();
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
            while (!ring.trySend(std::move(value))) {
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
            auto value = ring.tryRecv();
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
