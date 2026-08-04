/**
 * @file t153_spsc_unbounded.cc
 * @brief 验证跨线程分块 SPSC unbounded channel 的 FIFO、批量和 waiter 唤醒。
 */

#include "benchmark/cpp/common/benchmark_sync.h"
#include "result_writer.h"

#include <galay/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/core/compute_scheduler.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_failNothrowAllocation{false};

} // namespace

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (g_failNothrowAllocation.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return ::operator new(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
    return ::operator new(size, tag);
}

void operator delete(void* storage, const std::nothrow_t&) noexcept
{
    ::operator delete(storage);
}

void operator delete[](void* storage, const std::nothrow_t&) noexcept
{
    ::operator delete(storage);
}

namespace {

using namespace std::chrono_literals;

struct ThrowingMoveAssign
{
    ThrowingMoveAssign() = default;
    ThrowingMoveAssign(const ThrowingMoveAssign&) = delete;
    ThrowingMoveAssign& operator=(const ThrowingMoveAssign&) = delete;
    ThrowingMoveAssign(ThrowingMoveAssign&&) noexcept = default;
    ThrowingMoveAssign& operator=(ThrowingMoveAssign&&) noexcept(false)
    {
        return *this;
    }
};

template <typename T>
concept SupportsCallerOwnedBatch = requires(
    galay::spsc::UnboundedChannel<T>& channel, std::span<T> output) {
    { channel.tryRecvBatch(output) } -> std::same_as<size_t>;
    channel.recvBatchTo(output);
};

using UIntChannel = galay::spsc::UnboundedChannel<uint64_t>;
using UIntBatchToAwaitable = decltype(
    std::declval<UIntChannel&>().recvBatchTo(std::declval<std::span<uint64_t>>()));
using UIntBatchAwaitable = decltype(std::declval<UIntChannel&>().recvBatch(2));
using UIntBatchedAwaitable = decltype(std::declval<UIntChannel&>().recvBatched(2));
struct AwaitSuspendProbePromise {};

static_assert(galay::spsc::UnboundedValue<ThrowingMoveAssign>);
static_assert(SupportsCallerOwnedBatch<uint64_t>);
static_assert(SupportsCallerOwnedBatch<std::unique_ptr<int>>);
static_assert(!SupportsCallerOwnedBatch<ThrowingMoveAssign>);
static_assert(noexcept(std::declval<UIntChannel&>().tryRecvBatch(
    std::declval<std::span<uint64_t>>())));
static_assert(noexcept(std::declval<UIntChannel&>().recvBatchTo(
    std::declval<std::span<uint64_t>>())));
static_assert(noexcept(std::declval<UIntBatchToAwaitable&>().await_ready()));
static_assert(noexcept(std::declval<UIntBatchToAwaitable&>().await_suspend(
    std::coroutine_handle<AwaitSuspendProbePromise>{})));
static_assert(noexcept(std::declval<UIntBatchToAwaitable&>().await_resume()));
static_assert(!noexcept(std::declval<UIntBatchAwaitable&>().await_ready()));
static_assert(!noexcept(std::declval<UIntBatchAwaitable&>().await_suspend(
    std::coroutine_handle<AwaitSuspendProbePromise>{})));
static_assert(!noexcept(std::declval<UIntBatchAwaitable&>().await_resume()));
static_assert(!noexcept(std::declval<UIntBatchedAwaitable&>().await_ready()));
static_assert(!noexcept(std::declval<UIntBatchedAwaitable&>().await_suspend(
    std::coroutine_handle<AwaitSuspendProbePromise>{})));
static_assert(!noexcept(std::declval<UIntBatchedAwaitable&>().await_resume()));

bool waitFor(const std::atomic<bool>& flag, std::chrono::milliseconds timeout = 5s)
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

bool testCrossThreadFifo()
{
    constexpr uint64_t kMessages = 500'000;
    galay::spsc::UnboundedChannel<uint64_t> channel(galay::spsc::WakeMode::Deferred);
    galay::benchmark::CompletionLatch ready(2);
    galay::benchmark::StartGate start;
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> produced{0};
    std::atomic<uint64_t> consumed{0};
    std::atomic<uint64_t> badExpected{0};
    std::atomic<uint64_t> badActual{0};

    std::thread producer([&]() {
        ready.arrive();
        start.wait();
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (uint64_t sequence = 0; sequence < kMessages; ++sequence) {
            uint64_t value = sequence;
            if (!channel.send(std::move(value))) {
                failed.store(true, std::memory_order_release);
                return;
            }
            produced.store(sequence + 1, std::memory_order_release);
            if (std::chrono::steady_clock::now() >= deadline) {
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    });

    std::thread consumer([&]() {
        ready.arrive();
        start.wait();
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        uint64_t expected = 0;
        while (expected < kMessages && !failed.load(std::memory_order_acquire)) {
            auto value = channel.tryRecv();
            if (!value.has_value()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
                continue;
            }
            if (*value != expected) {
                badExpected.store(expected, std::memory_order_relaxed);
                badActual.store(*value, std::memory_order_relaxed);
                failed.store(true, std::memory_order_release);
                return;
            }
            ++expected;
            consumed.store(expected, std::memory_order_release);
        }
    });

    if (!ready.waitFor(2s)) {
        failed.store(true, std::memory_order_release);
    }
    start.open();
    producer.join();
    consumer.join();

    const bool passed = !failed.load(std::memory_order_acquire) && channel.empty() &&
        produced.load(std::memory_order_acquire) == kMessages &&
        consumed.load(std::memory_order_acquire) == kMessages;
    if (!passed) {
        std::cerr << "SPSC unbounded FIFO failed: produced="
                  << produced.load(std::memory_order_relaxed)
                  << " consumed=" << consumed.load(std::memory_order_relaxed)
                  << " expected=" << badExpected.load(std::memory_order_relaxed)
                  << " actual=" << badActual.load(std::memory_order_relaxed) << '\n';
    }
    return passed;
}

bool testMoveOnlyBatch()
{
    galay::spsc::UnboundedChannel<std::unique_ptr<int>> channel;
    std::vector<std::unique_ptr<int>> values;
    values.reserve(10'000);
    for (int value = 0; value < 10'000; ++value) {
        values.push_back(std::make_unique<int>(value));
    }
    if (!channel.sendBatch(std::move(values))) {
        return false;
    }

    auto first = channel.tryRecvBatch(4'321);
    auto second = channel.tryRecvBatch(10'000);
    if (!first.has_value() || !second.has_value() || first->size() != 4'321 ||
        second->size() != 5'679) {
        return false;
    }
    for (size_t i = 0; i < first->size(); ++i) {
        if (!(*first)[i] || *(*first)[i] != static_cast<int>(i)) {
            return false;
        }
    }
    for (size_t i = 0; i < second->size(); ++i) {
        const size_t expected = i + first->size();
        if (!(*second)[i] || *(*second)[i] != static_cast<int>(expected)) {
            return false;
        }
    }
    return channel.empty();
}

bool testCallerOwnedBatch()
{
    using Value = std::unique_ptr<int>;
    galay::spsc::UnboundedChannel<Value> channel;

    std::span<Value> emptyOutput;
    auto emptyReceive = channel.recvBatchTo(emptyOutput);
    if (!emptyReceive.await_ready()) {
        return false;
    }
    auto emptyResult = emptyReceive.await_resume();
    if (!emptyResult.has_value() || *emptyResult != 0) {
        return false;
    }

    std::vector<Value> values;
    values.reserve(5);
    for (int value = 0; value < 5; ++value) {
        values.push_back(std::make_unique<int>(value));
    }
    if (!channel.sendBatch(std::move(values))) {
        return false;
    }

    std::array<Value, 3> first;
    const size_t firstCount = channel.tryRecvBatch(std::span<Value>(first));
    if (firstCount != first.size()) {
        return false;
    }
    for (size_t index = 0; index < firstCount; ++index) {
        if (!first[index] || *first[index] != static_cast<int>(index)) {
            return false;
        }
    }

    std::array<Value, 3> second;
    auto secondReceive = channel.recvBatchTo(std::span<Value>(second));
    if (!secondReceive.await_ready()) {
        return false;
    }
    auto secondResult = secondReceive.await_resume();
    return secondResult.has_value() && *secondResult == 2 && second[0] &&
        *second[0] == 3 && second[1] && *second[1] == 4 && !second[2] &&
        channel.empty();
}

bool testCallerOwnedBatchAcrossBlocks()
{
    using Value = std::unique_ptr<int>;
    constexpr size_t kMessages = 2'053;
    constexpr size_t kFirstBatch = 2'048;
    galay::spsc::UnboundedChannel<Value> channel;

    std::vector<Value> values;
    values.reserve(kMessages);
    for (size_t index = 0; index < kMessages; ++index) {
        values.push_back(std::make_unique<int>(static_cast<int>(index)));
    }
    if (!channel.sendBatch(std::move(values))) {
        return false;
    }

    std::vector<Value> first(kFirstBatch);
    const size_t firstCount = channel.tryRecvBatch(std::span<Value>(first));
    if (firstCount != first.size()) {
        return false;
    }
    for (size_t index = 0; index < firstCount; ++index) {
        if (!first[index] || *first[index] != static_cast<int>(index)) {
            return false;
        }
    }

    std::array<Value, 8> tail;
    const size_t tailCount = channel.tryRecvBatch(std::span<Value>(tail));
    if (tailCount != kMessages - kFirstBatch) {
        return false;
    }
    for (size_t index = 0; index < tailCount; ++index) {
        const size_t expected = kFirstBatch + index;
        if (!tail[index] || *tail[index] != static_cast<int>(expected)) {
            return false;
        }
    }
    for (size_t index = tailCount; index < tail.size(); ++index) {
        if (tail[index]) {
            return false;
        }
    }
    return channel.empty();
}

bool testConstructionOomIsRecoverable()
{
    g_failNothrowAllocation.store(true, std::memory_order_release);
    galay::spsc::UnboundedQueue<int> queue;
    galay::spsc::UnboundedChannel<int> channel;
    g_failNothrowAllocation.store(false, std::memory_order_release);
    int queueValue = 72;
    if (queue.valid() || queue.send(std::move(queueValue)) ||
        channel.valid() || channel.send(73)) {
        return false;
    }

    auto receive = channel.recv();
    if (!receive.await_ready()) {
        return false;
    }
    auto receiveResult = receive.await_resume();
    if (receiveResult.has_value() || !galay::kernel::IOError::contains(
            receiveResult.error().code(), galay::kernel::kOutOfMemory) ||
        receiveResult.error().message() != "Out of memory (sys: no error)") {
        return false;
    }

    auto batch = channel.recvBatch(2);
    if (!batch.await_ready()) {
        return false;
    }
    auto batchResult = batch.await_resume();
    if (batchResult.has_value() || !galay::kernel::IOError::contains(
            batchResult.error().code(), galay::kernel::kOutOfMemory)) {
        return false;
    }

    std::array<int, 2> output{-1, -1};
    auto batchTo = channel.recvBatchTo(std::span<int>(output));
    if (!batchTo.await_ready()) {
        return false;
    }
    auto batchToResult = batchTo.await_resume();
    if (batchToResult.has_value() || output != std::array<int, 2>{-1, -1} ||
        !galay::kernel::IOError::contains(
            batchToResult.error().code(), galay::kernel::kOutOfMemory)) {
        return false;
    }

    auto batched = channel.recvBatched(2);
    if (!batched.await_ready()) {
        return false;
    }
    auto batchedResult = batched.await_resume();
    return !batchedResult.has_value() && galay::kernel::IOError::contains(
        batchedResult.error().code(), galay::kernel::kOutOfMemory);
}

bool testQueueGrowthOomIsRecoverable()
{
    galay::spsc::UnboundedQueue<size_t> queue;
    if (!queue.valid()) {
        return false;
    }

    const size_t blockCapacity = queue.blockCapacity();
    for (size_t value = 0; value < blockCapacity; ++value) {
        size_t pending = value;
        if (!queue.send(std::move(pending))) {
            return false;
        }
    }

    size_t rejected = blockCapacity;
    g_failNothrowAllocation.store(true, std::memory_order_release);
    const bool allocationFailed = !queue.send(std::move(rejected));
    g_failNothrowAllocation.store(false, std::memory_order_release);
    if (!allocationFailed || rejected != blockCapacity ||
        queue.size() != blockCapacity) {
        return false;
    }

    for (size_t expected = 0; expected < blockCapacity; ++expected) {
        auto value = queue.tryRecv();
        if (!value.has_value() || *value != expected) {
            return false;
        }
    }

    size_t recovered = blockCapacity;
    if (!queue.send(std::move(recovered))) {
        return false;
    }
    auto value = queue.tryRecv();
    return value.has_value() && *value == blockCapacity && queue.empty();
}

bool testConcurrentValidSnapshot()
{
    galay::spsc::UnboundedChannel<uint64_t> channel;
    if (!channel.valid()) {
        return false;
    }

    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::thread observer([&]() {
        started.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) {
            if (!channel.valid()) {
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
        if (!channel.send(std::move(value))) {
            failed.store(true, std::memory_order_release);
            break;
        }
    }
    done.store(true, std::memory_order_release);
    observer.join();
    return !failed.load(std::memory_order_acquire);
}

struct AsyncState
{
    std::atomic<bool> entered{false};
    std::atomic<bool> done{false};
    bool success = false;
    uint64_t error = 0;
    int value = 0;
};

struct AsyncBatchState
{
    size_t count = 0;
    std::array<int, 4> values{-1, -1, -1, -1};
    std::atomic<bool> entered{false};
    std::atomic<bool> done{false};
    bool success = false;
};

galay::kernel::Task<void> receiveAcrossThread(
    galay::spsc::UnboundedChannel<int>* channel,
    AsyncState* state)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->recv();
    if (result.has_value()) {
        state->success = true;
        state->value = *result;
    } else {
        state->error = result.error().code();
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

galay::kernel::Task<void> receiveBatchAcrossThread(
    galay::spsc::UnboundedChannel<int>* channel,
    AsyncBatchState* state)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->recvBatchTo(std::span<int>(state->values));
    if (result.has_value()) {
        state->success = true;
        state->count = *result;
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

bool testCrossThreadWaiterWake()
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    galay::kernel::ComputeScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }

    AsyncState state;
    if (!galay::kernel::scheduleTask(scheduler, receiveAcrossThread(&channel, &state)) ||
        !waitFor(state.entered)) {
        scheduler.stop();
        return false;
    }

    std::thread producer([&]() {
        int value = 42;
        if (!channel.send(std::move(value), true)) {
            return;
        }
    });
    producer.join();
    const bool done = waitFor(state.done);
    scheduler.stop();
    return done && state.success && state.value == 42 && channel.empty();
}

bool testCallerOwnedBatchWaiterWake()
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    galay::kernel::ComputeScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }

    AsyncBatchState state;
    if (!galay::kernel::scheduleTask(
            scheduler, receiveBatchAcrossThread(&channel, &state)) ||
        !waitFor(state.entered)) {
        scheduler.stop();
        return false;
    }

    std::vector<int> values{41, 42, 43};
    const bool sent = channel.sendBatch(std::move(values), true);
    const bool done = waitFor(state.done);
    scheduler.stop();
    return sent && done && state.success && state.count == 3 &&
        state.values == std::array<int, 4>{41, 42, 43, -1} && channel.empty();
}

bool testWaiterRegistrationRetainsTaskReference()
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Inline);
    AsyncState state;
    auto task = receiveAcrossThread(&channel, &state);
    galay::kernel::TaskRef keeper = galay::kernel::detail::TaskAccess::taskRef(task);
    auto* taskState = keeper.state();
    if (taskState == nullptr || !taskState->m_handle) {
        return false;
    }

    const uint64_t refsBeforeSuspend =
        taskState->m_refs.load(std::memory_order_acquire);
    taskState->m_handle.resume();
    const uint64_t refsWhileRegistered =
        taskState->m_refs.load(std::memory_order_acquire);

    int value = 73;
    const bool sent = channel.send(std::move(value), true);
    return state.entered.load(std::memory_order_acquire) &&
        refsWhileRegistered == refsBeforeSuspend + 1 && sent &&
        state.done.load(std::memory_order_acquire) && state.success && state.value == 73;
}

bool testSecondWaiterDoesNotDisturbRegisteredWaiter()
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Inline);
    AsyncState firstState;
    AsyncState secondState;
    auto firstTask = receiveAcrossThread(&channel, &firstState);
    auto secondTask = receiveAcrossThread(&channel, &secondState);
    galay::kernel::TaskRef firstKeeper =
        galay::kernel::detail::TaskAccess::taskRef(firstTask);
    galay::kernel::TaskRef secondKeeper =
        galay::kernel::detail::TaskAccess::taskRef(secondTask);
    auto* firstTaskState = firstKeeper.state();
    auto* secondTaskState = secondKeeper.state();
    if (firstTaskState == nullptr || secondTaskState == nullptr ||
        !firstTaskState->m_handle || !secondTaskState->m_handle) {
        return false;
    }

    firstTaskState->m_handle.resume();
    secondTaskState->m_handle.resume();
    const bool secondRejected = secondState.done.load(std::memory_order_acquire) &&
        !secondState.success && galay::kernel::IOError::contains(
            secondState.error, galay::kernel::kNotReady);
    if (!secondRejected || firstState.done.load(std::memory_order_acquire)) {
        return false;
    }

    int value = 91;
    const bool sent = channel.send(std::move(value), true);
    return sent && firstState.done.load(std::memory_order_acquire) &&
        firstState.success && firstState.value == 91 && channel.empty();
}

} // namespace

int main()
{
    galay::test::TestResultWriter writer("t153_spsc_unbounded");
    const bool fifoPassed = testCrossThreadFifo();
    const bool batchPassed = testMoveOnlyBatch();
    const bool callerOwnedBatchPassed = testCallerOwnedBatch();
    const bool callerOwnedAcrossBlocksPassed = testCallerOwnedBatchAcrossBlocks();
    const bool constructionOomPassed = testConstructionOomIsRecoverable();
    const bool growthOomPassed = testQueueGrowthOomIsRecoverable();
    const bool concurrentValidPassed = testConcurrentValidSnapshot();
    const bool wakePassed = testCrossThreadWaiterWake();
    const bool callerOwnedWakePassed = testCallerOwnedBatchWaiterWake();
    const bool retainedRefPassed = testWaiterRegistrationRetainsTaskReference();
    const bool doubleWaiterPassed =
        testSecondWaiterDoesNotDisturbRegisteredWaiter();
    const bool passed = fifoPassed && batchPassed && callerOwnedBatchPassed &&
        callerOwnedAcrossBlocksPassed && wakePassed && callerOwnedWakePassed &&
        retainedRefPassed && doubleWaiterPassed && constructionOomPassed &&
        growthOomPassed && concurrentValidPassed;

    writer.addTest();
    if (passed) {
        writer.addPassed();
    } else {
        writer.addFailed();
    }
    writer.writeResult();

    std::cout << "cross_thread_fifo=" << (fifoPassed ? "PASS" : "FAIL") << '\n'
              << "move_only_batch=" << (batchPassed ? "PASS" : "FAIL") << '\n'
              << "caller_owned_batch=" << (callerOwnedBatchPassed ? "PASS" : "FAIL")
              << '\n'
              << "caller_owned_across_blocks="
              << (callerOwnedAcrossBlocksPassed ? "PASS" : "FAIL") << '\n'
              << "construction_oom=" << (constructionOomPassed ? "PASS" : "FAIL")
              << '\n'
              << "growth_oom=" << (growthOomPassed ? "PASS" : "FAIL") << '\n'
              << "concurrent_valid_snapshot="
              << (concurrentValidPassed ? "PASS" : "FAIL") << '\n'
              << "cross_thread_waiter=" << (wakePassed ? "PASS" : "FAIL") << '\n'
              << "caller_owned_waiter=" << (callerOwnedWakePassed ? "PASS" : "FAIL")
              << '\n'
              << "waiter_registration_ref=" << (retainedRefPassed ? "PASS" : "FAIL")
              << '\n'
              << "double_waiter_rejected=" << (doubleWaiterPassed ? "PASS" : "FAIL")
              << '\n';
    return passed ? 0 : 1;
}
