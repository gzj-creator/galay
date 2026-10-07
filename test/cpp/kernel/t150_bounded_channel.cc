/**
 * @file t150_bounded_channel.cc
 * @brief 有界 MPMC channel 边界测试。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/bounded_channel.h>
#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "result_writer.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

struct TestState {
    std::atomic<bool> entered{false};
    std::atomic<bool> done{false};
    bool success = false;
    bool closed = false;
    bool timedOut = false;
    int count = 0;
    int value = 0;
};

struct NonDefaultMoveOnly {
    explicit NonDefaultMoveOnly(int input, int* destruction_count = nullptr)
        : destructionCount(destruction_count), value(input) {}

    NonDefaultMoveOnly() = delete;
    NonDefaultMoveOnly(const NonDefaultMoveOnly&) = delete;
    NonDefaultMoveOnly& operator=(const NonDefaultMoveOnly&) = delete;
    NonDefaultMoveOnly(NonDefaultMoveOnly&& other) noexcept
        : destructionCount(other.destructionCount), value(other.value)
    {
        other.destructionCount = nullptr;
    }
    NonDefaultMoveOnly& operator=(NonDefaultMoveOnly&& other) noexcept
    {
        if (this != &other) {
            value = other.value;
            destructionCount = other.destructionCount;
            other.destructionCount = nullptr;
        }
        return *this;
    }

    ~NonDefaultMoveOnly()
    {
        if (destructionCount != nullptr) {
            ++*destructionCount;
        }
    }

    int* destructionCount;
    int value;
};

struct ThrowingMoveValue {
    ThrowingMoveValue() = default;
    ThrowingMoveValue(const ThrowingMoveValue&) = delete;
    ThrowingMoveValue& operator=(const ThrowingMoveValue&) = delete;
    ThrowingMoveValue(ThrowingMoveValue&&) noexcept(false) {}
    ThrowingMoveValue& operator=(ThrowingMoveValue&&) noexcept = default;
};

struct ThrowingMoveAssignValue {
    ThrowingMoveAssignValue() = default;
    ThrowingMoveAssignValue(ThrowingMoveAssignValue&&) noexcept = default;
    ThrowingMoveAssignValue& operator=(ThrowingMoveAssignValue&&) noexcept(false)
    {
        return *this;
    }
};

struct ThrowingCopyValue {
    ThrowingCopyValue() = default;
    ThrowingCopyValue(const ThrowingCopyValue&) noexcept(false) {}
    ThrowingCopyValue(ThrowingCopyValue&&) noexcept = default;
    ThrowingCopyValue& operator=(ThrowingCopyValue&&) noexcept = default;
};

template <typename T>
concept HasBoundedCopySend = requires(galay::mpmc::BoundedChannel<T>& channel,
                                      const T& value) {
    channel.try_send(value);
};

static_assert(galay::mpmc::BoundedValue<NonDefaultMoveOnly>);
static_assert(std::movable<ThrowingMoveValue>);
static_assert(std::movable<ThrowingMoveAssignValue>);
static_assert(std::copy_constructible<ThrowingCopyValue>);
static_assert(!galay::mpmc::BoundedValue<ThrowingMoveValue>);
static_assert(galay::mpmc::BoundedValue<ThrowingMoveAssignValue>);
static_assert(galay::mpmc::BoundedValue<ThrowingCopyValue>);
static_assert(HasBoundedCopySend<int>);
static_assert(!HasBoundedCopySend<ThrowingCopyValue>);

bool wait_for(const std::atomic<bool>& flag, std::chrono::milliseconds timeout = 2s)
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

Task<void> receive_one(galay::mpmc::BoundedChannel<int>* channel, TestState* state)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->recv();
    if (result) {
        state->success = true;
        state->value = *result;
    } else {
        state->closed = IOError::contains(result.error().code(), kClosed);
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

Task<void> send_one(galay::mpmc::BoundedChannel<int>* channel, TestState* state, int value)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->send(std::move(value));
    state->success = result.has_value();
    state->closed = !result && IOError::contains(result.error().code(), kClosed);
    state->done.store(true, std::memory_order_release);
    co_return;
}

Task<void> receive_with_timeout(galay::mpmc::BoundedChannel<int>* channel, TestState* state)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->recv().timeout(2ms);
    state->timedOut = !result && IOError::contains(result.error().code(), kTimeout);
    state->done.store(true, std::memory_order_release);
    co_return;
}

Task<void> send_with_timeout(galay::mpmc::BoundedChannel<int>* channel, TestState* state, int value)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->send(std::move(value)).timeout(2ms);
    state->success = result.has_value();
    state->timedOut = !result && IOError::contains(result.error().code(), kTimeout);
    state->done.store(true, std::memory_order_release);
    co_return;
}

Task<void> receive_batch(galay::mpmc::BoundedChannel<int>* channel, TestState* state, size_t count)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->recv_batch(count);
    if (result) {
        state->success = true;
        state->count = static_cast<int>(result->size());
        if (!result->empty()) {
            state->value = result->front();
        }
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

Task<void> send_unique(galay::mpmc::BoundedChannel<std::unique_ptr<int>>* channel, TestState* state)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->send(std::make_unique<int>(123));
    state->success = result.has_value();
    state->done.store(true, std::memory_order_release);
    co_return;
}

bool schedule_and_wait(ParallelScheduler& scheduler, Task<void>&& task, TestState& state)
{
    if (!schedule_task(scheduler, std::move(task))) {
        return false;
    }
    return wait_for(state.done);
}

bool test_basic_and_capacity()
{
    galay::mpmc::BoundedChannel<int> channel(3);
    if (channel.capacity() != 4 || galay::mpmc::BoundedChannel<int>(1).capacity() != 2 ||
        galay::mpmc::BoundedChannel<int>(0).capacity() != 2 || galay::mpmc::BoundedChannel<int>(1024).capacity() != 1024) {
        return false;
    }
    if (!channel.try_send(42)) {
        return false;
    }
    auto value = channel.try_recv();
    return value.has_value() && *value == 42 && channel.empty();
}

bool test_full_preserves_value()
{
    galay::mpmc::BoundedChannel<std::string> channel(2);
    if (!channel.try_send(std::string("first")) || !channel.try_send(std::string("second"))) {
        return false;
    }
    std::string pending = "pending";
    if (channel.try_send(std::move(pending))) {
        return false;
    }
    return pending == "pending" && channel.full();
}

bool test_async_send_wake()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!channel.try_send(1) || !channel.try_send(2)) {
        return false;
    }

    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    if (!schedule_task(scheduler, send_one(&channel, &state, 3)) || !wait_for(state.entered)) {
        scheduler.stop();
        return false;
    }
    if (state.done.load(std::memory_order_acquire)) {
        scheduler.stop();
        return false;
    }
    auto first = channel.try_recv();
    const bool woke = wait_for(state.done);
    scheduler.stop();
    auto second = channel.try_recv();
    return first.has_value() && woke && state.success && second.has_value() && *second == 2;
}

bool test_async_receive_wake()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    if (!schedule_task(scheduler, receive_one(&channel, &state)) || !wait_for(state.entered)) {
        scheduler.stop();
        return false;
    }
    if (state.done.load(std::memory_order_acquire) || !channel.try_send(7)) {
        scheduler.stop();
        return false;
    }
    const bool woke = wait_for(state.done);
    scheduler.stop();
    return woke && state.success && state.value == 7;
}

bool test_batch_and_minimum_capacity()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!channel.try_recv_batch(0).has_value()) {
        return false;
    }
    for (int i = 0; i < 5000; ++i) {
        while (!channel.try_send(i)) {
            if (!channel.try_recv().has_value()) {
                return false;
            }
        }
        auto value = channel.try_recv();
        if (!value.has_value() || *value != i) {
            return false;
        }
    }

    galay::mpmc::BoundedChannel<int> batchChannel(8);
    for (int i = 0; i < 5; ++i) {
        if (!batchChannel.try_send(i)) {
            return false;
        }
    }
    auto first = batchChannel.try_recv_batch(3);
    auto second = batchChannel.try_recv_batch(3);
    if (!first || !second || first->size() != 3 || second->size() != 2 ||
        (*first)[0] != 0 || (*first)[2] != 2 || (*second)[0] != 3 || (*second)[1] != 4) {
        return false;
    }

    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    if (!schedule_task(scheduler, receive_batch(&batchChannel, &state, 4)) ||
        !wait_for(state.entered) || !batchChannel.try_send(8)) {
        scheduler.stop();
        return false;
    }
    const bool done = wait_for(state.done);
    scheduler.stop();
    return done && state.success && state.count == 1 && state.value == 8;
}

bool test_close_and_drain()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!channel.try_send(10) || !channel.try_send(11)) {
        return false;
    }
    channel.close();
    channel.close();
    if (!channel.is_closed() || channel.try_send(12)) {
        return false;
    }
    auto first = channel.try_recv();
    auto second = channel.try_recv();
    auto third = channel.try_recv();
    if (!first || !second || third || *first != 10 || *second != 11) {
        return false;
    }

    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    const bool scheduled = schedule_task(scheduler, receive_one(&channel, &state));
    const bool done = scheduled && wait_for(state.done);
    scheduler.stop();
    channel.close();
    return done && state.closed &&
           !IOError::contains(IOError(kClosed, 0).code(), kTimeout) &&
           IOError(kClosed, 0).message().find("Channel closed") != std::string::npos;
}

bool test_close_wakes_pending_send()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!channel.try_send(1) || !channel.try_send(2)) {
        return false;
    }
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    if (!schedule_task(scheduler, send_one(&channel, &state, 3)) || !wait_for(state.entered)) {
        scheduler.stop();
        return false;
    }
    channel.close();
    const bool done = wait_for(state.done);
    scheduler.stop();
    return done && state.closed && !state.success;
}

bool test_close_wakes_pending_receive()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    if (!schedule_task(scheduler, receive_one(&channel, &state)) || !wait_for(state.entered)) {
        scheduler.stop();
        return false;
    }
    channel.close();
    const bool done = wait_for(state.done);
    scheduler.stop();
    return done && state.closed && !state.success;
}

bool test_timeout()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    const bool scheduled = schedule_task(scheduler, receive_with_timeout(&channel, &state));
    const bool done = scheduled && wait_for(state.done);
    scheduler.stop();
    return done && state.timedOut;
}

bool test_send_timeout()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!channel.try_send(1) || !channel.try_send(2)) {
        return false;
    }
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    const bool scheduled = schedule_task(scheduler, send_with_timeout(&channel, &state, 3));
    const bool done = scheduled && wait_for(state.done);
    scheduler.stop();
    return done && state.timedOut && !state.success;
}

bool test_move_only()
{
    galay::mpmc::BoundedChannel<std::unique_ptr<int>> channel(2);
    auto value = std::make_unique<int>(99);
    if (!channel.try_send(std::move(value)) || value) {
        return false;
    }
    auto received = channel.try_recv();
    if (!received.has_value() || !*received || **received != 99) {
        return false;
    }

    galay::mpmc::BoundedChannel<std::unique_ptr<int>> asyncChannel(2);
    if (!asyncChannel.try_send(std::make_unique<int>(1)) ||
        !asyncChannel.try_send(std::make_unique<int>(2))) {
        return false;
    }
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    if (!schedule_task(scheduler, send_unique(&asyncChannel, &state)) || !wait_for(state.entered)) {
        scheduler.stop();
        return false;
    }
    const auto first = asyncChannel.try_recv();
    const bool done = first.has_value() && wait_for(state.done);
    scheduler.stop();
    return done && state.success;
}

bool test_non_default_move_only()
{
    int destructionCount = 0;
    {
        galay::mpmc::BoundedChannel<NonDefaultMoveOnly> channel(2);
        if (!channel.try_send(NonDefaultMoveOnly(17, &destructionCount))) {
            return false;
        }
        auto value = channel.try_recv();
        if (!value.has_value() || value->value != 17) {
            return false;
        }
    }
    if (destructionCount != 1) {
        return false;
    }

    destructionCount = 0;
    {
        galay::mpmc::BoundedChannel<NonDefaultMoveOnly> channel(2);
        if (!channel.try_send(NonDefaultMoveOnly(23, &destructionCount))) {
            return false;
        }
    }
    return destructionCount == 1;
}

bool test_mpmc()
{
    constexpr int producerCount = 4;
    constexpr int consumerCount = 4;
    constexpr int messagesPerProducer = 10000;
    constexpr int totalMessages = producerCount * messagesPerProducer;

    for (const size_t capacity : {size_t{256}, size_t{4096}}) {
        galay::mpmc::BoundedChannel<int> channel(capacity);
        std::atomic<int> receivedCount{0};
        std::atomic<bool> producersDone{false};
        std::set<int> received;
        std::mutex receivedMutex;
        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;

        for (int producer = 0; producer < producerCount; ++producer) {
            producers.emplace_back([&channel, producer]() {
                for (int sequence = 0; sequence < messagesPerProducer; ++sequence) {
                    const int value = producer * messagesPerProducer + sequence;
                    while (!channel.try_send(value)) {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (int consumer = 0; consumer < consumerCount; ++consumer) {
            consumers.emplace_back([&]() {
                for (;;) {
                    auto value = channel.try_recv();
                    if (value.has_value()) {
                        {
                            std::lock_guard lock(receivedMutex);
                            received.insert(*value);
                        }
                        receivedCount.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    if (producersDone.load(std::memory_order_acquire) &&
                        receivedCount.load(std::memory_order_acquire) == totalMessages) {
                        return;
                    }
                    std::this_thread::yield();
                }
            });
        }

        for (auto& producer : producers) {
            producer.join();
        }
        producersDone.store(true, std::memory_order_release);
        for (auto& consumer : consumers) {
            consumer.join();
        }
        if (receivedCount.load(std::memory_order_acquire) != totalMessages ||
            received.size() != static_cast<size_t>(totalMessages)) {
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    galay::test::TestResultWriter resultWriter("test_bounded_channel");
    const std::vector<std::pair<const char*, bool (*)()>> tests = {
        {"basic_and_capacity", test_basic_and_capacity},
        {"full_preserves_value", test_full_preserves_value},
        {"async_send_wake", test_async_send_wake},
        {"async_receive_wake", test_async_receive_wake},
        {"batch_and_minimum_capacity", test_batch_and_minimum_capacity},
        {"close_and_drain", test_close_and_drain},
        {"close_wakes_pending_send", test_close_wakes_pending_send},
        {"close_wakes_pending_receive", test_close_wakes_pending_receive},
        {"timeout", test_timeout},
        {"send_timeout", test_send_timeout},
        {"move_only", test_move_only},
        {"non_default_move_only", test_non_default_move_only},
        {"mpmc", test_mpmc},
    };

    bool allPassed = true;
    for (const auto& [name, test] : tests) {
        const bool passed = test();
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
        allPassed = passed && allPassed;
    }
    resultWriter.add_test();
    if (allPassed) {
        resultWriter.add_passed();
    } else {
        resultWriter.add_failed();
    }
    resultWriter.write_result();
    return allPassed ? 0 : 1;
}
