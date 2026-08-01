/**
 * @file t165_spsc_unbounded_queue.cc
 * @brief 验证无 waiter 的 SPSC unbounded 数据面在跨块、批量和跨线程场景保持 FIFO。
 */

#include "result_writer.h"

#include <galay/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace {

using galay::spsc::UnboundedQueue;
using galay::spsc::UnboundedQueueValue;
using galay::spsc::UnboundedValue;
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
concept SupportsConstUnboundedSend = requires(
    galay::spsc::UnboundedChannel<T>& channel, const T& value) {
    { channel.send(value) } -> std::same_as<bool>;
};

template <typename T>
concept SupportsConstUnboundedBatchSend = requires(
    galay::spsc::UnboundedChannel<T>& channel, const std::vector<T>& values) {
    { channel.sendBatch(values) } -> std::same_as<bool>;
};

static_assert(UnboundedQueueValue<std::unique_ptr<int>>);
static_assert(!UnboundedQueueValue<ThrowingMove>);
static_assert(!UnboundedValue<ThrowingMove>);
static_assert(UnboundedValue<ThrowingCopy>);
static_assert(!SupportsConstUnboundedSend<ThrowingCopy>);
static_assert(!SupportsConstUnboundedBatchSend<ThrowingCopy>);
static_assert(!UnboundedQueueValue<int&>);
static_assert(!UnboundedQueueValue<const int>);

bool testMoveOnlyAcrossBlocks()
{
    UnboundedQueue<std::unique_ptr<int>> queue;
    if (!queue.valid()) {
        return false;
    }

    constexpr int kMessages = 4'097;
    for (int value = 0; value < kMessages; ++value) {
        auto item = std::make_unique<int>(value);
        if (!queue.send(std::move(item)) || item) {
            return false;
        }
    }
    for (int expected = 0; expected < kMessages; ++expected) {
        auto item = queue.tryRecv();
        if (!item.has_value() || !*item || **item != expected) {
            return false;
        }
    }
    return queue.empty();
}

bool testCallerOwnedBatch()
{
    UnboundedQueue<uint64_t> queue;
    if (!queue.valid()) {
        return false;
    }

    std::vector<uint64_t> source(2'050);
    for (size_t index = 0; index < source.size(); ++index) {
        source[index] = index + 11;
    }
    if (!queue.sendBatch(std::span<uint64_t>(source))) {
        return false;
    }

    std::array<uint64_t, 257> output{};
    size_t received = 0;
    while (received < source.size()) {
        const size_t count = queue.tryRecvBatch(std::span<uint64_t>(output));
        if (count == 0 || count > output.size()) {
            return false;
        }
        for (size_t index = 0; index < count; ++index) {
            if (output[index] != received + index + 11) {
                return false;
            }
        }
        received += count;
    }
    return queue.tryRecvBatch(std::span<uint64_t>(output)) == 0 && queue.empty();
}

bool testCallerOwnedSingleReceive()
{
    UnboundedQueue<int> queue;
    int input = 91;
    int output = -1;
    return queue.valid() && queue.send(std::move(input)) &&
        queue.tryRecv(output) && output == 91 && !queue.tryRecv(output) &&
        output == 91;
}

bool testConcurrentValidSnapshot()
{
    UnboundedQueue<uint64_t> queue;
    if (!queue.valid()) {
        return false;
    }

    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::thread observer([&]() {
        started.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) {
            if (!queue.valid()) {
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    for (uint64_t sequence = 0; sequence < 100'000; ++sequence) {
        uint64_t value = sequence;
        if (!queue.send(std::move(value))) {
            failed.store(true, std::memory_order_release);
            break;
        }
    }
    done.store(true, std::memory_order_release);
    observer.join();
    return !failed.load(std::memory_order_acquire);
}

bool testCrossThreadFifo()
{
    constexpr uint64_t kMessages = 500'000;
    UnboundedQueue<uint64_t> queue;
    if (!queue.valid()) {
        return false;
    }

    std::atomic<bool> failed{false};
    std::atomic<uint64_t> consumed{0};
    std::thread producer([&]() {
        for (uint64_t sequence = 0; sequence < kMessages; ++sequence) {
            uint64_t value = sequence;
            if (!queue.send(std::move(value))) {
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    });

    std::thread consumer([&]() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        uint64_t expected = 0;
        while (expected < kMessages) {
            auto value = queue.tryRecv();
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
        consumed.store(expected, std::memory_order_release);
    });

    producer.join();
    consumer.join();
    return !failed.load(std::memory_order_acquire) && queue.empty() &&
        consumed.load(std::memory_order_acquire) == kMessages;
}

} // namespace

int main()
{
    const std::array results{
        std::pair{"move_only_across_blocks", testMoveOnlyAcrossBlocks()},
        std::pair{"caller_owned_batch", testCallerOwnedBatch()},
        std::pair{"caller_owned_single_receive", testCallerOwnedSingleReceive()},
        std::pair{"concurrent_valid_snapshot", testConcurrentValidSnapshot()},
        std::pair{"cross_thread_fifo", testCrossThreadFifo()},
    };

    galay::test::TestResultWriter writer("t165_spsc_unbounded_queue");
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
