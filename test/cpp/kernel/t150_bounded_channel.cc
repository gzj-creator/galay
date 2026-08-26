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
    channel.trySend(value);
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

bool waitFor(const std::atomic<bool>& flag, std::chrono::milliseconds timeout = 2s)
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

Task<void> receiveOne(galay::mpmc::BoundedChannel<int>* channel, TestState* state)
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

Task<void> sendOne(galay::mpmc::BoundedChannel<int>* channel, TestState* state, int value)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->send(std::move(value));
    state->success = result.has_value();
    state->closed = !result && IOError::contains(result.error().code(), kClosed);
    state->done.store(true, std::memory_order_release);
    co_return;
}

Task<void> receiveWithTimeout(galay::mpmc::BoundedChannel<int>* channel, TestState* state)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->recv().timeout(2ms);
    state->timedOut = !result && IOError::contains(result.error().code(), kTimeout);
    state->done.store(true, std::memory_order_release);
    co_return;
}

Task<void> sendWithTimeout(galay::mpmc::BoundedChannel<int>* channel, TestState* state, int value)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->send(std::move(value)).timeout(2ms);
    state->success = result.has_value();
    state->timedOut = !result && IOError::contains(result.error().code(), kTimeout);
    state->done.store(true, std::memory_order_release);
    co_return;
}

Task<void> receiveBatch(galay::mpmc::BoundedChannel<int>* channel, TestState* state, size_t count)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->recvBatch(count);
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

Task<void> sendUnique(galay::mpmc::BoundedChannel<std::unique_ptr<int>>* channel, TestState* state)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->send(std::make_unique<int>(123));
    state->success = result.has_value();
    state->done.store(true, std::memory_order_release);
    co_return;
}

bool scheduleAndWait(ParallelScheduler& scheduler, Task<void>&& task, TestState& state)
{
    if (!scheduleTask(scheduler, std::move(task))) {
        return false;
    }
    return waitFor(state.done);
}

bool testBasicAndCapacity()
{
    galay::mpmc::BoundedChannel<int> channel(3);
    if (channel.capacity() != 4 || galay::mpmc::BoundedChannel<int>(1).capacity() != 2 ||
        galay::mpmc::BoundedChannel<int>(0).capacity() != 2 || galay::mpmc::BoundedChannel<int>(1024).capacity() != 1024) {
        return false;
    }
    if (!channel.trySend(42)) {
        return false;
    }
    auto value = channel.tryRecv();
    return value.has_value() && *value == 42 && channel.empty();
}

bool testFullPreservesValue()
{
    galay::mpmc::BoundedChannel<std::string> channel(2);
    if (!channel.trySend(std::string("first")) || !channel.trySend(std::string("second"))) {
        return false;
    }
    std::string pending = "pending";
    if (channel.trySend(std::move(pending))) {
        return false;
    }
    return pending == "pending" && channel.full();
}

bool testAsyncSendWake()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!channel.trySend(1) || !channel.trySend(2)) {
        return false;
    }

    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    if (!scheduleTask(scheduler, sendOne(&channel, &state, 3)) || !waitFor(state.entered)) {
        scheduler.stop();
        return false;
    }
    if (state.done.load(std::memory_order_acquire)) {
        scheduler.stop();
        return false;
    }
    auto first = channel.tryRecv();
    const bool woke = waitFor(state.done);
    scheduler.stop();
    auto second = channel.tryRecv();
    return first.has_value() && woke && state.success && second.has_value() && *second == 2;
}

bool testAsyncReceiveWake()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    if (!scheduleTask(scheduler, receiveOne(&channel, &state)) || !waitFor(state.entered)) {
        scheduler.stop();
        return false;
    }
    if (state.done.load(std::memory_order_acquire) || !channel.trySend(7)) {
        scheduler.stop();
        return false;
    }
    const bool woke = waitFor(state.done);
    scheduler.stop();
    return woke && state.success && state.value == 7;
}

bool testBatchAndMinimumCapacity()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!channel.tryRecvBatch(0).has_value()) {
        return false;
    }
    for (int i = 0; i < 5000; ++i) {
        while (!channel.trySend(i)) {
            if (!channel.tryRecv().has_value()) {
                return false;
            }
        }
        auto value = channel.tryRecv();
        if (!value.has_value() || *value != i) {
            return false;
        }
    }

    galay::mpmc::BoundedChannel<int> batchChannel(8);
    for (int i = 0; i < 5; ++i) {
        if (!batchChannel.trySend(i)) {
            return false;
        }
    }
    auto first = batchChannel.tryRecvBatch(3);
    auto second = batchChannel.tryRecvBatch(3);
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
    if (!scheduleTask(scheduler, receiveBatch(&batchChannel, &state, 4)) ||
        !waitFor(state.entered) || !batchChannel.trySend(8)) {
        scheduler.stop();
        return false;
    }
    const bool done = waitFor(state.done);
    scheduler.stop();
    return done && state.success && state.count == 1 && state.value == 8;
}

bool testCloseAndDrain()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!channel.trySend(10) || !channel.trySend(11)) {
        return false;
    }
    channel.close();
    channel.close();
    if (!channel.isClosed() || channel.trySend(12)) {
        return false;
    }
    auto first = channel.tryRecv();
    auto second = channel.tryRecv();
    auto third = channel.tryRecv();
    if (!first || !second || third || *first != 10 || *second != 11) {
        return false;
    }

    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    const bool scheduled = scheduleTask(scheduler, receiveOne(&channel, &state));
    const bool done = scheduled && waitFor(state.done);
    scheduler.stop();
    channel.close();
    return done && state.closed &&
           !IOError::contains(IOError(kClosed, 0).code(), kTimeout) &&
           IOError(kClosed, 0).message().find("Channel closed") != std::string::npos;
}

bool testCloseWakesPendingSend()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!channel.trySend(1) || !channel.trySend(2)) {
        return false;
    }
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    if (!scheduleTask(scheduler, sendOne(&channel, &state, 3)) || !waitFor(state.entered)) {
        scheduler.stop();
        return false;
    }
    channel.close();
    const bool done = waitFor(state.done);
    scheduler.stop();
    return done && state.closed && !state.success;
}

bool testCloseWakesPendingReceive()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    if (!scheduleTask(scheduler, receiveOne(&channel, &state)) || !waitFor(state.entered)) {
        scheduler.stop();
        return false;
    }
    channel.close();
    const bool done = waitFor(state.done);
    scheduler.stop();
    return done && state.closed && !state.success;
}

bool testTimeout()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    const bool scheduled = scheduleTask(scheduler, receiveWithTimeout(&channel, &state));
    const bool done = scheduled && waitFor(state.done);
    scheduler.stop();
    return done && state.timedOut;
}

bool testSendTimeout()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!channel.trySend(1) || !channel.trySend(2)) {
        return false;
    }
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    const bool scheduled = scheduleTask(scheduler, sendWithTimeout(&channel, &state, 3));
    const bool done = scheduled && waitFor(state.done);
    scheduler.stop();
    return done && state.timedOut && !state.success;
}

bool testMoveOnly()
{
    galay::mpmc::BoundedChannel<std::unique_ptr<int>> channel(2);
    auto value = std::make_unique<int>(99);
    if (!channel.trySend(std::move(value)) || value) {
        return false;
    }
    auto received = channel.tryRecv();
    if (!received.has_value() || !*received || **received != 99) {
        return false;
    }

    galay::mpmc::BoundedChannel<std::unique_ptr<int>> asyncChannel(2);
    if (!asyncChannel.trySend(std::make_unique<int>(1)) ||
        !asyncChannel.trySend(std::make_unique<int>(2))) {
        return false;
    }
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    TestState state;
    if (!scheduleTask(scheduler, sendUnique(&asyncChannel, &state)) || !waitFor(state.entered)) {
        scheduler.stop();
        return false;
    }
    const auto first = asyncChannel.tryRecv();
    const bool done = first.has_value() && waitFor(state.done);
    scheduler.stop();
    return done && state.success;
}

bool testNonDefaultMoveOnly()
{
    int destructionCount = 0;
    {
        galay::mpmc::BoundedChannel<NonDefaultMoveOnly> channel(2);
        if (!channel.trySend(NonDefaultMoveOnly(17, &destructionCount))) {
            return false;
        }
        auto value = channel.tryRecv();
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
        if (!channel.trySend(NonDefaultMoveOnly(23, &destructionCount))) {
            return false;
        }
    }
    return destructionCount == 1;
}

bool testMpmc()
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
                    while (!channel.trySend(value)) {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (int consumer = 0; consumer < consumerCount; ++consumer) {
            consumers.emplace_back([&]() {
                for (;;) {
                    auto value = channel.tryRecv();
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
        {"basic_and_capacity", testBasicAndCapacity},
        {"full_preserves_value", testFullPreservesValue},
        {"async_send_wake", testAsyncSendWake},
        {"async_receive_wake", testAsyncReceiveWake},
        {"batch_and_minimum_capacity", testBatchAndMinimumCapacity},
        {"close_and_drain", testCloseAndDrain},
        {"close_wakes_pending_send", testCloseWakesPendingSend},
        {"close_wakes_pending_receive", testCloseWakesPendingReceive},
        {"timeout", testTimeout},
        {"send_timeout", testSendTimeout},
        {"move_only", testMoveOnly},
        {"non_default_move_only", testNonDefaultMoveOnly},
        {"mpmc", testMpmc},
    };

    bool allPassed = true;
    for (const auto& [name, test] : tests) {
        const bool passed = test();
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
        allPassed = passed && allPassed;
    }
    resultWriter.addTest();
    if (allPassed) {
        resultWriter.addPassed();
    } else {
        resultWriter.addFailed();
    }
    resultWriter.writeResult();
    return allPassed ? 0 : 1;
}
