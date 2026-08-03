/**
 * @file t19_spsc_ring_buffer.cc
 * @brief 验证 typed SPSC ring 的容量、移动语义、批量接口、回绕与跨线程 FIFO。
 */

#include <galay/cpp/galay-utils/cache/type_ring_buffer.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using galay::utils::SpscRingBuffer;
using galay::utils::SpscRingBufferCursor;
using galay::utils::SpscRingBufferError;
using galay::utils::SpscRingBufferValue;
using galay::utils::StaticSpscRingBuffer;
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

static_assert(SpscRingBufferValue<std::unique_ptr<int>>);
static_assert(!SpscRingBufferValue<ThrowingMove>);
static_assert(!SpscRingBufferValue<int&>);
static_assert(!SpscRingBufferValue<const int>);
static_assert(SpscRingBufferCursor<uint8_t>);
static_assert(!SpscRingBufferCursor<bool>);

template <size_t Capacity, typename Cursor = size_t>
concept SupportsStaticCapacity = requires {
    typename SpscRingBuffer<int, Capacity, Cursor>;
};

static_assert(SupportsStaticCapacity<2>);
static_assert(!SupportsStaticCapacity<1>);
static_assert(!SupportsStaticCapacity<3>);
static_assert(SupportsStaticCapacity<128, uint8_t>);
static_assert(!SupportsStaticCapacity<256, uint8_t>);
static_assert(std::same_as<StaticSpscRingBuffer<int, 4, uint8_t>,
                           SpscRingBuffer<int, 4, uint8_t>>);
static_assert(std::is_default_constructible_v<StaticSpscRingBuffer<int, 4>>);
static_assert(!std::is_constructible_v<StaticSpscRingBuffer<int, 4>, size_t>);
static_assert(std::is_constructible_v<SpscRingBuffer<int>, size_t>);
static_assert(!std::is_default_constructible_v<SpscRingBuffer<int>>);
static_assert(sizeof(StaticSpscRingBuffer<uint64_t, 64>) >
              sizeof(StaticSpscRingBuffer<uint64_t, 2>));
using IntRing = SpscRingBuffer<int>;
static_assert(std::is_move_constructible_v<typename IntRing::Producer>);
static_assert(std::is_nothrow_move_assignable_v<typename IntRing::Producer>);
static_assert(!std::is_copy_constructible_v<typename IntRing::Producer>);
static_assert(std::is_move_constructible_v<typename IntRing::Consumer>);
static_assert(std::is_nothrow_move_assignable_v<typename IntRing::Consumer>);
static_assert(!std::is_copy_constructible_v<typename IntRing::Consumer>);

bool testErrorsAndCapacity()
{
    SpscRingBuffer<int> rounded(3);
    SpscRingBuffer<int, std::dynamic_extent, uint8_t> tooLarge(129);
    return rounded.error() == SpscRingBufferError::kNone &&
        rounded.capacity() == 4 &&
        tooLarge.error() == SpscRingBufferError::kCapacityTooLarge &&
        tooLarge.capacity() == 0 &&
        std::string_view(galay::utils::spscRingBufferErrorString(
            SpscRingBufferError::kNone)) == "none" &&
        std::string_view(galay::utils::spscRingBufferErrorString(
            SpscRingBufferError::kCapacityTooLarge)) == "capacity too large" &&
        std::string_view(galay::utils::spscRingBufferErrorString(
            SpscRingBufferError::kAllocationFailed)) == "allocation failed";
}

bool testFullCapacityMoveOnlyAndBatch()
{
    SpscRingBuffer<std::unique_ptr<int>> ring(4);
    if (ring.error() != SpscRingBufferError::kNone) {
        return false;
    }
    for (int value = 0; value < 4; ++value) {
        auto item = std::make_unique<int>(value);
        if (!ring.trySend(std::move(item)) || item != nullptr) {
            return false;
        }
    }
    auto rejected = std::make_unique<int>(99);
    if (ring.trySend(std::move(rejected)) || rejected == nullptr ||
        *rejected != 99) {
        return false;
    }

    for (int expected = 0; expected < 4; ++expected) {
        auto item = ring.tryRecv();
        if (!item.has_value() || *item == nullptr || **item != expected) {
            return false;
        }
    }

    SpscRingBuffer<int> batchRing(8);
    std::array<int, 6> input{10, 11, 12, 13, 14, 15};
    std::array<int, 4> first{};
    std::array<int, 4> second{};
    if (batchRing.trySendBatch(std::span<int>(input)) != input.size() ||
        batchRing.tryRecvBatch(std::span<int>(first)) != first.size() ||
        batchRing.tryRecvBatch(std::span<int>(second)) != 2 ||
        batchRing.tryRecvBatch(std::span<int>(second)) != 0) {
        return false;
    }
    return first == std::array<int, 4>{10, 11, 12, 13} &&
        second[0] == 14 && second[1] == 15;
}

bool testNarrowCursorWraparound()
{
    SpscRingBuffer<uint16_t, std::dynamic_extent, uint8_t> ring(4);
    for (uint16_t sequence = 0; sequence < 320; ++sequence) {
        uint16_t value = sequence;
        if (!ring.trySend(std::move(value))) {
            return false;
        }
        uint16_t output = 0;
        if (!ring.tryRecv(output) || output != sequence) {
            return false;
        }
    }
    return !ring.tryRecv().has_value();
}

bool testStaticCapacityStorageAndFifo()
{
    StaticSpscRingBuffer<std::unique_ptr<int>, 4> ring;
    if (ring.error() != SpscRingBufferError::kNone || ring.capacity() != 4) {
        return false;
    }
    for (int value = 0; value < 4; ++value) {
        auto item = std::make_unique<int>(value);
        if (!ring.trySend(std::move(item)) || item != nullptr) {
            return false;
        }
    }
    auto rejected = std::make_unique<int>(99);
    if (ring.trySend(std::move(rejected)) || rejected == nullptr ||
        *rejected != 99) {
        return false;
    }
    for (int expected = 0; expected < 4; ++expected) {
        auto item = ring.tryRecv();
        if (!item.has_value() || *item == nullptr || **item != expected) {
            return false;
        }
    }

    StaticSpscRingBuffer<int, 8> batchRing;
    std::array<int, 6> input{10, 11, 12, 13, 14, 15};
    std::array<int, 6> output{};
    return batchRing.trySendBatch(std::span<int>(input)) == input.size() &&
        batchRing.tryRecvBatch(std::span<int>(output)) == output.size() &&
        input == output;
}

bool testStaticNarrowCursorWraparound()
{
    StaticSpscRingBuffer<uint16_t, 4, uint8_t> ring;
    for (uint16_t sequence = 0; sequence < 320; ++sequence) {
        uint16_t value = sequence;
        uint16_t output = 0;
        if (!ring.trySend(std::move(value)) || !ring.tryRecv(output) ||
            output != sequence) {
            return false;
        }
    }
    return !ring.tryRecv().has_value();
}

bool testCrossThreadFifo()
{
    constexpr uint32_t kMessages = 250'000;
    SpscRingBuffer<uint32_t> ring(256);
    if (ring.error() != SpscRingBufferError::kNone) {
        return false;
    }
    auto endpoints = ring.split();

    std::atomic<bool> failed{false};
    std::thread producer([
        endpoint = std::move(endpoints.producer), &failed]() mutable {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (uint32_t sequence = 0; sequence < kMessages; ++sequence) {
            uint32_t value = sequence;
            while (!endpoint.trySend(std::move(value))) {
                if (failed.load(std::memory_order_acquire) ||
                    std::chrono::steady_clock::now() >= deadline) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([
        endpoint = std::move(endpoints.consumer), &failed]() mutable {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        uint32_t expected = 0;
        while (expected < kMessages) {
            uint32_t value = 0;
            if (!endpoint.tryRecv(value)) {
                if (failed.load(std::memory_order_acquire) ||
                    std::chrono::steady_clock::now() >= deadline) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
                continue;
            }
            if (value != expected) {
                failed.store(true, std::memory_order_release);
                return;
            }
            ++expected;
        }
        uint32_t extra = 0;
        if (endpoint.tryRecv(extra)) {
            failed.store(true, std::memory_order_release);
        }
    });

    producer.join();
    consumer.join();
    return !failed.load(std::memory_order_acquire);
}

} // namespace

int main()
{
    const std::array results{
        std::pair{"errors_and_capacity", testErrorsAndCapacity()},
        std::pair{"full_capacity_move_only_batch", testFullCapacityMoveOnlyAndBatch()},
        std::pair{"narrow_cursor_wraparound", testNarrowCursorWraparound()},
        std::pair{"static_capacity_storage_fifo", testStaticCapacityStorageAndFifo()},
        std::pair{"static_narrow_cursor_wraparound", testStaticNarrowCursorWraparound()},
        std::pair{"cross_thread_fifo", testCrossThreadFifo()},
    };
    bool passed = true;
    for (const auto& [name, result] : results) {
        std::cout << name << '=' << (result ? "PASS" : "FAIL") << '\n';
        passed = passed && result;
    }
    return passed ? 0 : 1;
}
