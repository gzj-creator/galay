/**
 * @file t19_spsc_ring_buffer.cc
 * @brief 验证 typed SPSC ring 的容量、移动语义、批量接口、回绕与跨线程 FIFO。
 */

#include <galay/cpp/galay-utils/buffer/type_ring_buffer.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using galay::utils::TypeRingBuffer;
using galay::utils::TypeRingBufferError;
using galay::utils::StaticTypeRingBuffer;
using galay::utils::TypeRingBufferCursor;
using galay::utils::TypeRingBufferValue;
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

static_assert(TypeRingBufferValue<std::unique_ptr<int>>);
static_assert(!TypeRingBufferValue<ThrowingMove>);
static_assert(!TypeRingBufferValue<int&>);
static_assert(!TypeRingBufferValue<const int>);
static_assert(TypeRingBufferCursor<uint8_t>);
static_assert(!TypeRingBufferCursor<bool>);
#if defined(__aarch64__) || defined(__ARM_ARCH_ISA_A64) || \
    defined(_M_ARM64) || defined(_M_ARM64EC)
static_assert(galay::utils::kCacheLineSize == 128);
#else
static_assert(galay::utils::kCacheLineSize == 64);
#endif

template <size_t Capacity, typename Cursor = size_t>
concept SupportsStaticCapacity = requires {
    typename TypeRingBuffer<int, Capacity, Cursor>;
};

static_assert(SupportsStaticCapacity<2>);
static_assert(!SupportsStaticCapacity<1>);
static_assert(!SupportsStaticCapacity<3>);
static_assert(SupportsStaticCapacity<128, uint8_t>);
static_assert(!SupportsStaticCapacity<256, uint8_t>);
static_assert(std::same_as<StaticTypeRingBuffer<int, 4, uint8_t>,
                           TypeRingBuffer<int, 4, uint8_t>>);
static_assert(std::is_default_constructible_v<StaticTypeRingBuffer<int, 4>>);
static_assert(!std::is_constructible_v<StaticTypeRingBuffer<int, 4>, size_t>);
static_assert(std::is_constructible_v<TypeRingBuffer<int>, size_t>);
static_assert(!std::is_default_constructible_v<TypeRingBuffer<int>>);
static_assert(sizeof(StaticTypeRingBuffer<uint64_t, 64>) >
              sizeof(StaticTypeRingBuffer<uint64_t, 2>));
using IntRing = TypeRingBuffer<int>;

template <typename Ring>
concept UnifiedTypedRingApi = requires(
    Ring& ring, int value, std::span<int> values) {
    { ring.try_write(std::move(value)) } -> std::same_as<bool>;
    { ring.try_read() } -> std::same_as<std::optional<int>>;
    { ring.try_read(value) } -> std::same_as<bool>;
    { ring.try_write_batch(values) } -> std::same_as<size_t>;
    { ring.try_read_batch(values) } -> std::same_as<size_t>;
};

template <typename Ring>
concept HasLegacyTrySend = requires(Ring& ring, int value) {
    ring.try_send(std::move(value));
};

template <typename Ring>
concept HasLegacyTryRecvValue = requires(Ring& ring) {
    ring.try_recv();
};

template <typename Ring>
concept HasLegacyTryRecvTo = requires(Ring& ring, int value) {
    ring.try_recv(value);
};

template <typename Ring>
concept HasLegacyTrySendBatch = requires(Ring& ring, std::span<int> values) {
    ring.trySendBatch(values);
};

template <typename Ring>
concept HasLegacyTryRecvBatch = requires(Ring& ring, std::span<int> values) {
    ring.try_recv_batch(values);
};

template <typename Producer>
concept UnifiedProducerApi = requires(
    Producer& producer, int value, std::span<int> values) {
    { producer.try_write(std::move(value)) } -> std::same_as<bool>;
    { producer.try_write_batch(values) } -> std::same_as<size_t>;
};

template <typename Consumer>
concept UnifiedConsumerApi = requires(
    Consumer& consumer, int value, std::span<int> values) {
    { consumer.try_read() } -> std::same_as<std::optional<int>>;
    { consumer.try_read(value) } -> std::same_as<bool>;
    { consumer.try_read_batch(values) } -> std::same_as<size_t>;
};

template <typename Producer>
concept HasLegacyProducerTrySend = requires(Producer& producer, int value) {
    producer.try_send(std::move(value));
};

template <typename Producer>
concept HasLegacyProducerTrySendBatch = requires(
    Producer& producer, std::span<int> values) {
    producer.trySendBatch(values);
};

template <typename Consumer>
concept HasLegacyConsumerTryRecvValue = requires(Consumer& consumer) {
    consumer.try_recv();
};

template <typename Consumer>
concept HasLegacyConsumerTryRecvTo = requires(Consumer& consumer, int value) {
    consumer.try_recv(value);
};

template <typename Consumer>
concept HasLegacyConsumerTryRecvBatch = requires(
    Consumer& consumer, std::span<int> values) {
    consumer.try_recv_batch(values);
};

static_assert(UnifiedTypedRingApi<IntRing>);
static_assert(!HasLegacyTrySend<IntRing>);
static_assert(!HasLegacyTryRecvValue<IntRing>);
static_assert(!HasLegacyTryRecvTo<IntRing>);
static_assert(!HasLegacyTrySendBatch<IntRing>);
static_assert(!HasLegacyTryRecvBatch<IntRing>);
static_assert(UnifiedProducerApi<typename IntRing::Producer>);
static_assert(UnifiedConsumerApi<typename IntRing::Consumer>);
static_assert(!HasLegacyProducerTrySend<typename IntRing::Producer>);
static_assert(!HasLegacyProducerTrySendBatch<typename IntRing::Producer>);
static_assert(!HasLegacyConsumerTryRecvValue<typename IntRing::Consumer>);
static_assert(!HasLegacyConsumerTryRecvTo<typename IntRing::Consumer>);
static_assert(!HasLegacyConsumerTryRecvBatch<typename IntRing::Consumer>);
static_assert(std::is_move_constructible_v<typename IntRing::Producer>);
static_assert(std::is_nothrow_move_assignable_v<typename IntRing::Producer>);
static_assert(!std::is_copy_constructible_v<typename IntRing::Producer>);
static_assert(std::is_move_constructible_v<typename IntRing::Consumer>);
static_assert(std::is_nothrow_move_assignable_v<typename IntRing::Consumer>);
static_assert(!std::is_copy_constructible_v<typename IntRing::Consumer>);

bool test_errors_and_capacity()
{
    TypeRingBuffer<int> rounded(3);
    TypeRingBuffer<int, std::dynamic_extent, uint8_t> tooLarge(129);
    return rounded.error() == TypeRingBufferError::kNone &&
        rounded.capacity() == 4 &&
        tooLarge.error() == TypeRingBufferError::kCapacityTooLarge &&
        tooLarge.capacity() == 0 &&
        std::string_view(galay::utils::type_ring_buffer_error_string(
            TypeRingBufferError::kNone)) == "none" &&
        std::string_view(galay::utils::type_ring_buffer_error_string(
            TypeRingBufferError::kCapacityTooLarge)) == "capacity too large" &&
        std::string_view(galay::utils::type_ring_buffer_error_string(
            TypeRingBufferError::kAllocationFailed)) == "allocation failed";
}

bool test_full_capacity_move_only_and_batch()
{
    TypeRingBuffer<std::unique_ptr<int>> ring(4);
    if (ring.error() != TypeRingBufferError::kNone) {
        return false;
    }
    for (int value = 0; value < 4; ++value) {
        auto item = std::make_unique<int>(value);
        if (!ring.try_write(std::move(item)) || item != nullptr) {
            return false;
        }
    }
    auto rejected = std::make_unique<int>(99);
    if (ring.try_write(std::move(rejected)) || rejected == nullptr ||
        *rejected != 99) {
        return false;
    }

    for (int expected = 0; expected < 4; ++expected) {
        auto item = ring.try_read();
        if (!item.has_value() || *item == nullptr || **item != expected) {
            return false;
        }
    }

    TypeRingBuffer<int> batchRing(8);
    std::array<int, 6> input{10, 11, 12, 13, 14, 15};
    std::array<int, 4> first{};
    std::array<int, 4> second{};
    if (batchRing.try_write_batch(std::span<int>(input)) != input.size() ||
        batchRing.try_read_batch(std::span<int>(first)) != first.size() ||
        batchRing.try_read_batch(std::span<int>(second)) != 2 ||
        batchRing.try_read_batch(std::span<int>(second)) != 0) {
        return false;
    }
    return first == std::array<int, 4>{10, 11, 12, 13} &&
        second[0] == 14 && second[1] == 15;
}

bool test_narrow_cursor_wraparound()
{
    TypeRingBuffer<uint16_t, std::dynamic_extent, uint8_t> ring(4);
    for (uint16_t sequence = 0; sequence < 320; ++sequence) {
        uint16_t value = sequence;
        if (!ring.try_write(std::move(value))) {
            return false;
        }
        uint16_t output = 0;
        if (!ring.try_read(output) || output != sequence) {
            return false;
        }
    }
    return !ring.try_read().has_value();
}

bool test_static_capacity_storage_and_fifo()
{
    StaticTypeRingBuffer<std::unique_ptr<int>, 4> ring;
    if (ring.error() != TypeRingBufferError::kNone || ring.capacity() != 4) {
        return false;
    }
    for (int value = 0; value < 4; ++value) {
        auto item = std::make_unique<int>(value);
        if (!ring.try_write(std::move(item)) || item != nullptr) {
            return false;
        }
    }
    auto rejected = std::make_unique<int>(99);
    if (ring.try_write(std::move(rejected)) || rejected == nullptr ||
        *rejected != 99) {
        return false;
    }
    for (int expected = 0; expected < 4; ++expected) {
        auto item = ring.try_read();
        if (!item.has_value() || *item == nullptr || **item != expected) {
            return false;
        }
    }

    StaticTypeRingBuffer<int, 8> batchRing;
    std::array<int, 6> input{10, 11, 12, 13, 14, 15};
    std::array<int, 6> output{};
    return batchRing.try_write_batch(std::span<int>(input)) == input.size() &&
        batchRing.try_read_batch(std::span<int>(output)) == output.size() &&
        input == output;
}

bool test_static_narrow_cursor_wraparound()
{
    StaticTypeRingBuffer<uint16_t, 4, uint8_t> ring;
    for (uint16_t sequence = 0; sequence < 320; ++sequence) {
        uint16_t value = sequence;
        uint16_t output = 0;
        if (!ring.try_write(std::move(value)) || !ring.try_read(output) ||
            output != sequence) {
            return false;
        }
    }
    return !ring.try_read().has_value();
}

bool test_cross_thread_fifo()
{
    constexpr uint32_t kMessages = 250'000;
    TypeRingBuffer<uint32_t> ring(256);
    if (ring.error() != TypeRingBufferError::kNone) {
        return false;
    }
    auto endpoints = ring.split();

    std::atomic<bool> failed{false};
    std::thread producer([
        endpoint = std::move(endpoints.producer), &failed]() mutable {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (uint32_t sequence = 0; sequence < kMessages; ++sequence) {
            uint32_t value = sequence;
            while (!endpoint.try_write(std::move(value))) {
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
            if (!endpoint.try_read(value)) {
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
        if (endpoint.try_read(extra)) {
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
        std::pair{"errors_and_capacity", test_errors_and_capacity()},
        std::pair{"full_capacity_move_only_batch", test_full_capacity_move_only_and_batch()},
        std::pair{"narrow_cursor_wraparound", test_narrow_cursor_wraparound()},
        std::pair{"static_capacity_storage_fifo", test_static_capacity_storage_and_fifo()},
        std::pair{"static_narrow_cursor_wraparound", test_static_narrow_cursor_wraparound()},
        std::pair{"cross_thread_fifo", test_cross_thread_fifo()},
    };
    bool passed = true;
    for (const auto& [name, result] : results) {
        std::cout << name << '=' << (result ? "PASS" : "FAIL") << '\n';
        passed = passed && result;
    }
    return passed ? 0 : 1;
}
