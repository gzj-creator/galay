/**
 * @file t160_spsc_bounded_timeout_race.cc
 * @brief 验证 SPSC bounded 三种 awaitable 只由 channel 操作或 timeout 完成一次。
 */

#include <galay/cpp/galay-kernel/core/scheduler.hpp>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/timeout.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstdlib>
#include <cstddef>
#include <deque>
#include <expected>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace bounded_timeout_abort_test {
void pause(const void* waiter) noexcept;
}

namespace bounded_waiter_lease_test {
void pauseDequeued(const void* waiter) noexcept;
void observeReclaim(const void* waiter) noexcept;
}

// 精确停在 waiter 的 retry 最终检查与 timeout abort 之间，重放当前交错。
#define GALAY_SPSC_BOUNDED_TIMEOUT_ABORT_TEST_POINT(waiter) \
    ::bounded_timeout_abort_test::pause(static_cast<const void*>(waiter))
#define GALAY_SPSC_BOUNDED_DEQUEUE_TEST_POINT(waiter) \
    ::bounded_waiter_lease_test::pauseDequeued(static_cast<const void*>(waiter))
#define GALAY_SPSC_BOUNDED_RECLAIM_TEST_POINT(waiter) \
    ::bounded_waiter_lease_test::observeReclaim(static_cast<const void*>(waiter))
#define private public
#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>
#undef private
#undef GALAY_SPSC_BOUNDED_RECLAIM_TEST_POINT
#undef GALAY_SPSC_BOUNDED_DEQUEUE_TEST_POINT
#undef GALAY_SPSC_BOUNDED_TIMEOUT_ABORT_TEST_POINT

namespace {

std::atomic<size_t> g_allocationCount{0};
std::atomic<bool> g_failAllocation{false};

} // namespace

void* operator new(std::size_t size)
{
    if (g_failAllocation.load(std::memory_order_acquire)) {
        throw std::bad_alloc();
    }
    void* const storage = std::malloc(size == 0 ? 1 : size);
    if (storage == nullptr) {
        throw std::bad_alloc();
    }
    g_allocationCount.fetch_add(1, std::memory_order_relaxed);
    return storage;
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (g_failAllocation.load(std::memory_order_acquire)) {
        return nullptr;
    }
    void* const storage = std::malloc(size == 0 ? 1 : size);
    if (storage != nullptr) {
        g_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return storage;
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
    return ::operator new(size, tag);
}

void operator delete(void* storage) noexcept
{
    std::free(storage);
}

void operator delete[](void* storage) noexcept
{
    ::operator delete(storage);
}

void operator delete(void* storage, std::size_t) noexcept
{
    ::operator delete(storage);
}

void operator delete[](void* storage, std::size_t) noexcept
{
    ::operator delete(storage);
}

void operator delete(void* storage, const std::nothrow_t&) noexcept
{
    ::operator delete(storage);
}

void operator delete[](void* storage, const std::nothrow_t&) noexcept
{
    ::operator delete(storage);
}

namespace bounded_timeout_abort_test {

struct Gate
{
    std::atomic<bool> enabled{false};
    std::atomic<bool> arrived{false};
    std::atomic<bool> proceed{false};
};

Gate gate;

void reset() noexcept
{
    gate.arrived.store(false, std::memory_order_relaxed);
    gate.proceed.store(false, std::memory_order_relaxed);
    gate.enabled.store(true, std::memory_order_release);
}

void release() noexcept
{
    gate.enabled.store(false, std::memory_order_release);
    gate.proceed.store(true, std::memory_order_release);
}

void pause(const void*) noexcept
{
    if (!gate.enabled.load(std::memory_order_acquire)) {
        return;
    }
    gate.arrived.store(true, std::memory_order_release);
    while (!gate.proceed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

} // namespace bounded_timeout_abort_test

namespace bounded_waiter_lease_test {

struct Gate
{
    std::atomic<bool> enabled{false};
    std::atomic<bool> dequeued{false};
    std::atomic<bool> reclaiming{false};
    std::atomic<bool> proceed{false};
};

Gate gate;

void reset() noexcept
{
    gate.dequeued.store(false, std::memory_order_relaxed);
    gate.reclaiming.store(false, std::memory_order_relaxed);
    gate.proceed.store(false, std::memory_order_relaxed);
    gate.enabled.store(true, std::memory_order_release);
}

void release() noexcept
{
    gate.enabled.store(false, std::memory_order_release);
    gate.proceed.store(true, std::memory_order_release);
}

void pauseDequeued(const void*) noexcept
{
    if (!gate.enabled.load(std::memory_order_acquire)) {
        return;
    }
    gate.dequeued.store(true, std::memory_order_release);
    while (!gate.proceed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void observeReclaim(const void*) noexcept
{
    if (gate.enabled.load(std::memory_order_acquire)) {
        gate.reclaiming.store(true, std::memory_order_release);
    }
}

} // namespace bounded_waiter_lease_test

namespace {

using namespace galay::kernel;
using namespace std::chrono_literals;

enum class AwaitKind {
    kSend,
    kRecv,
    kRecvBatch,
    kRecvBatchTo,
};

struct LargeNothrowValue
{
    std::array<std::byte, 4096> bytes{};
};

static_assert(sizeof(galay::spsc::BoundedChannel<LargeNothrowValue>) ==
              sizeof(galay::spsc::BoundedChannel<int>),
              "fixed waiter control slots must not embed an extra T");

template <size_t Capacity>
concept SupportsStaticBoundedChannel = requires {
    typename galay::spsc::BoundedChannel<int, Capacity>;
};

using StaticBoundedChannel = galay::spsc::BoundedChannel<int, 4>;

static_assert(SupportsStaticBoundedChannel<2>);
static_assert(!SupportsStaticBoundedChannel<3>);
static_assert(std::is_constructible_v<galay::spsc::BoundedChannel<int>, size_t>);
static_assert(!std::is_default_constructible_v<galay::spsc::BoundedChannel<int>>);
static_assert(std::is_default_constructible_v<StaticBoundedChannel>);
static_assert(!std::is_constructible_v<StaticBoundedChannel, size_t>);
static_assert(std::same_as<
              decltype(std::declval<StaticBoundedChannel&>().send(1)),
              galay::spsc::BoundedSendAwaitable<int, 4>>);
static_assert(std::same_as<
              decltype(std::declval<StaticBoundedChannel&>().recv()),
              galay::spsc::BoundedRecvAwaitable<int, 4>>);
static_assert(std::same_as<
              decltype(std::declval<StaticBoundedChannel&>().recvBatch(2)),
              galay::spsc::BoundedRecvBatchAwaitable<int, 4>>);
static_assert(std::same_as<
              decltype(std::declval<StaticBoundedChannel&>().recvBatchTo(
                  std::declval<std::span<int>>())),
              galay::spsc::BoundedRecvBatchToAwaitable<int, 4>>);

constexpr std::array<AwaitKind, 4> kAwaitKinds{
    AwaitKind::kSend,
    AwaitKind::kRecv,
    AwaitKind::kRecvBatch,
    AwaitKind::kRecvBatchTo,
};

const char* awaitKindName(AwaitKind kind) noexcept
{
    switch (kind) {
    case AwaitKind::kSend:
        return "send";
    case AwaitKind::kRecv:
        return "recv";
    case AwaitKind::kRecvBatch:
        return "recvBatch";
    case AwaitKind::kRecvBatchTo:
        return "recvBatchTo";
    }
    return "unknown";
}

struct AwaitState
{
    std::atomic<size_t> valueCount{0};
    std::atomic<int> valueSum{0};
    std::atomic<int> resumeCount{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> operationSucceeded{false};
    std::atomic<bool> receivedTimeout{false};
    std::atomic<bool> receivedClosed{false};
    std::atomic<bool> receivedNotReady{false};
    std::atomic<bool> receivedOutOfMemory{false};
    std::atomic<bool> callerBufferContractOk{true};
    std::atomic<size_t> awaitAllocationCount{0};
};

bool waitForFlag(const std::atomic<bool>& flag,
                 std::chrono::milliseconds timeout = 5s)
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

struct BlockingMoveControl
{
    std::atomic<bool> moveStarted{false};
    std::atomic<bool> allowMove{false};
};

struct BlockingMove
{
    int value = 0;
    BlockingMoveControl* control = nullptr;

    BlockingMove(int input, BlockingMoveControl* moveControl) noexcept
        : value(input), control(moveControl)
    {
    }

    BlockingMove(const BlockingMove&) = delete;
    BlockingMove& operator=(const BlockingMove&) = delete;

    BlockingMove(BlockingMove&& other) noexcept
        : value(other.value), control(other.control)
    {
        waitForMovePermission();
        other.control = nullptr;
    }

    BlockingMove& operator=(BlockingMove&& other) noexcept
    {
        value = other.value;
        control = other.control;
        waitForMovePermission();
        other.control = nullptr;
        return *this;
    }

private:
    void waitForMovePermission() const noexcept
    {
        if (control == nullptr) {
            return;
        }
        control->moveStarted.store(true, std::memory_order_release);
        while (!control->allowMove.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};

struct BlockingRecvState
{
    std::atomic<bool> completed{false};
    bool succeeded = false;
    bool receivedClosed = false;
    int value = 0;
};

bool prepareChannel(galay::spsc::BoundedChannel<int>& channel, AwaitKind kind)
{
    if (kind != AwaitKind::kSend) {
        return true;
    }
    return channel.trySend(11) && channel.trySend(12) && channel.full();
}

bool completeWaitingOperation(galay::spsc::BoundedChannel<int>& channel,
                              AwaitKind kind)
{
    if (kind == AwaitKind::kSend) {
        auto value = channel.tryRecv();
        return value.has_value() && *value == 11;
    }
    return channel.trySend(73);
}

class ImmediateRaceScheduler final : public Scheduler
{
public:
    ImmediateRaceScheduler(galay::spsc::BoundedChannel<int>* channel,
                           AwaitKind kind,
                           bool completeOperation) noexcept
        : m_channel(channel), m_kind(kind), m_completeOperation(completeOperation)
    {
    }

    std::expected<void, IOError> start() override { return {}; }
    void stop() override {}

    bool schedule(TaskRef task) noexcept override
    {
        if (!bindTask(task)) {
            return false;
        }
        [[maybe_unused]] const int previousScheduleCalls =
            m_scheduleCalls.fetch_add(1, std::memory_order_relaxed);
        resume(task);
        return true;
    }

    // 兼容尚未提供专用 resume admission 的 Scheduler；新接口存在时仍会隐式覆盖。
    bool scheduleResume(TaskRef task) noexcept
    {
        return schedule(std::move(task));
    }

    bool scheduleDeferred(TaskRef task) noexcept override
    {
        return schedule(std::move(task));
    }

    bool scheduleImmediately(TaskRef task) noexcept override
    {
        if (!bindTask(task)) {
            return false;
        }
        resume(task);
        return true;
    }

    bool addTimer(Timer::ptr timer) override
    {
        [[maybe_unused]] const int previousAddTimerCalls =
            m_addTimerCalls.fetch_add(1, std::memory_order_relaxed);
        if (m_completeOperation) {
            m_actionSucceeded.store(
                completeWaitingOperation(*m_channel, m_kind),
                std::memory_order_release);
        }
        // 强制 WithTimeout 走 timeoutNow()，覆盖 inner 发布后的同步恢复窗口。
        return false;
    }

    SchedulerType type() override { return kParallelScheduler; }

    int scheduleCalls() const noexcept
    {
        return m_scheduleCalls.load(std::memory_order_acquire);
    }

    int addTimerCalls() const noexcept
    {
        return m_addTimerCalls.load(std::memory_order_acquire);
    }

    bool actionSucceeded() const noexcept
    {
        return m_actionSucceeded.load(std::memory_order_acquire);
    }

private:
    galay::spsc::BoundedChannel<int>* m_channel;
    const AwaitKind m_kind;
    const bool m_completeOperation;
    std::atomic<int> m_scheduleCalls{0};
    std::atomic<int> m_addTimerCalls{0};
    std::atomic<bool> m_actionSucceeded{false};
};

class QueuedRaceScheduler final : public Scheduler
{
public:
    std::expected<void, IOError> start() override { return {}; }
    void stop() override {}

    bool schedule(TaskRef task) noexcept override
    {
        if (!bindTask(task)) {
            return false;
        }
        m_ready.push_back(std::move(task));
        return true;
    }

    // 兼容尚未提供专用 resume admission 的 Scheduler；新接口存在时仍会隐式覆盖。
    bool scheduleResume(TaskRef task) noexcept
    {
        return schedule(std::move(task));
    }

    bool scheduleDeferred(TaskRef task) noexcept override
    {
        return schedule(std::move(task));
    }

    bool scheduleImmediately(TaskRef task) noexcept override
    {
        if (!bindTask(task)) {
            return false;
        }
        resume(task);
        return true;
    }

    bool addTimer(Timer::ptr timer) override
    {
        m_timer = std::move(timer);
        return true;
    }

    SchedulerType type() override { return kParallelScheduler; }

    bool bindForTest(TaskRef& task) { return bindTask(task); }

    bool hasSingleReadyTask() const noexcept { return m_ready.size() == 1; }

    bool fireTimer()
    {
        if (!m_timer) {
            return false;
        }
        m_timer->handleTimeout();
        return true;
    }

    bool runOne()
    {
        if (m_ready.empty()) {
            return false;
        }
        TaskRef task = std::move(m_ready.front());
        m_ready.pop_front();
        resume(task);
        return true;
    }

    bool resumeWithTimerInDequeueGap()
    {
        if (m_ready.empty() || !m_timer) {
            return false;
        }
        TaskRef task = std::move(m_ready.front());
        m_ready.pop_front();
        TaskState* state = task.state();
        if (state == nullptr || !state->m_handle ||
            state->m_done.load(std::memory_order_acquire)) {
            return false;
        }

        // 精确模拟 resumeTaskState() 在 dequeue 后、handle.resume() 前的窗口。
        state->m_queued.store(false, std::memory_order_relaxed);
        state->m_resume_owner_only.store(false, std::memory_order_relaxed);
        m_timer->handleTimeout();
        const bool noDuplicateWake = m_ready.empty();
        state->m_handle.resume();
        return noDuplicateWake;
    }

    void releaseRetainedState()
    {
        m_ready.clear();
        m_timer.reset();
    }

private:
    std::deque<TaskRef> m_ready;
    Timer::ptr m_timer;
};

Task<void> publicationTarget()
{
    co_return;
}

Task<void> receiveFromStaticChannel(StaticBoundedChannel* channel,
                                    AwaitState* state)
{
    auto result = co_await channel->recv();
    if (result.has_value()) {
        state->operationSucceeded.store(true, std::memory_order_release);
        state->valueCount.store(1, std::memory_order_release);
        state->valueSum.store(*result, std::memory_order_release);
    }
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    state->completed.store(true, std::memory_order_release);
    co_return;
}

bool runStaticCapacityChannel()
{
    const size_t allocationsBefore =
        g_allocationCount.load(std::memory_order_relaxed);
    StaticBoundedChannel channel;
    const size_t allocationsAfterConstruction =
        g_allocationCount.load(std::memory_order_relaxed);
    if (allocationsAfterConstruction != allocationsBefore ||
        channel.capacity() != 4 || !channel.empty()) {
        return false;
    }
    if (reinterpret_cast<uintptr_t>(&channel.m_slots) %
            galay::utils::kCacheLineSize != 0) {
        return false;
    }

    QueuedRaceScheduler scheduler;
    AwaitState state;
    auto task = receiveFromStaticChannel(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    if (!scheduler.scheduleImmediately(std::move(scheduled)) ||
        state.completed.load(std::memory_order_acquire) ||
        !channel.trySend(73) || !scheduler.hasSingleReadyTask() ||
        !scheduler.runOne()) {
        scheduler.releaseRetainedState();
        return false;
    }

    const bool completed =
        state.operationSucceeded.load(std::memory_order_acquire) &&
        state.valueCount.load(std::memory_order_acquire) == 1 &&
        state.valueSum.load(std::memory_order_acquire) == 73 &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.completed.load(std::memory_order_acquire) && channel.empty();
    scheduler.releaseRetainedState();
    return completed;
}

bool runPeerCompletionBeforeArmed(AwaitKind kind)
{
    galay::spsc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    auto target = publicationTarget();
    TaskRef keeper = detail::TaskAccess::taskRef(target);
    if (!scheduler.bindForTest(keeper)) {
        return false;
    }

    auto timer = std::make_shared<TimeoutTimer>(1h);
    auto* const waiter = kind == AwaitKind::kSend
        ? &channel.m_sendWaiterSlot
        : &channel.m_recvWaiterSlot;
    int sendValue = 73;
    std::optional<int> recvValue;
    uint64_t waiterGeneration = 0;
    if (!waiter->begin(Waker(keeper),
                       timer,
                       kind == AwaitKind::kSend ? &sendValue : nullptr,
                       kind == AwaitKind::kSend ? nullptr : &recvValue,
                       waiterGeneration)) {
        return false;
    }
    galay::spsc::BoundedWaiterProgress progress =
        galay::spsc::BoundedWaiterProgress::kNotClaimed;
    galay::spsc::BoundedChannel<int>::PendingWakes pendingWakes;
    bool channelStateCorrect = false;

    if (kind == AwaitKind::kSend) {
        if (!channel.trySend(11) || !channel.trySend(12)) {
            return false;
        }
        if (!channel.enqueueWaiter(channel.m_sendWaiters, waiter)) {
            return false;
        }
        int released = 0;
        if (!channel.ringDequeueTo([&released](int&& value) {
                released = value;
            })) {
            return false;
        }
        progress = channel.tryCompleteSendWaiter(waiter, true, pendingWakes);
        auto first = channel.tryRecv();
        auto second = channel.tryRecv();
        channelStateCorrect = released == 11 && first.has_value() && *first == 12 &&
            second.has_value() && *second == 73 && channel.empty();
    } else {
        if (!channel.enqueueWaiter(channel.m_recvWaiters, waiter)) {
            return false;
        }
        int value = 73;
        if (channel.ringEnqueue(std::move(value)) !=
            galay::spsc::BoundedChannel<int>::RingEnqueueResult::kPublished) {
            return false;
        }
        progress = channel.tryCompleteRecvWaiter(waiter, true, pendingWakes);
        channelStateCorrect = recvValue.has_value() && *recvValue == 73 &&
            channel.empty();
    }

    // 这里精确对应 await_suspend 已入队、但尚未执行最终 Armed 发布的窗口。
    // 对端可以提交结果，但不得恢复仍在 await_suspend 栈上的协程。
    const bool wakeDeferred = !scheduler.hasSingleReadyTask();
    const bool ownerContinuesSynchronously = !waiter->finishArming();
    pendingWakes.wakeAll();
    const bool reclaimed = kind == AwaitKind::kSend
        ? channel.reclaimWaiter(channel.m_sendWaiters, *waiter, waiterGeneration)
        : channel.reclaimWaiter(channel.m_recvWaiters, *waiter, waiterGeneration);
    scheduler.releaseRetainedState();
    return progress == galay::spsc::BoundedWaiterProgress::kCompleted &&
        waiter->state.load(std::memory_order_acquire) ==
            galay::spsc::BoundedWaiterState::kIdle &&
        wakeDeferred && ownerContinuesSynchronously && channelStateCorrect && reclaimed;
}

Task<void> awaitWithTimeout(galay::spsc::BoundedChannel<int>* channel,
                            AwaitKind kind,
                            AwaitState* state)
{
    if (kind == AwaitKind::kSend) {
        auto result = co_await channel->send(73).timeout(1h);
        state->operationSucceeded.store(result.has_value(), std::memory_order_release);
        if (!result.has_value()) {
            state->receivedTimeout.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
            state->receivedClosed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
        }
    } else if (kind == AwaitKind::kRecv) {
        auto result = co_await channel->recv().timeout(1h);
        if (result.has_value()) {
            state->operationSucceeded.store(true, std::memory_order_release);
            state->valueCount.store(1, std::memory_order_release);
            state->valueSum.store(*result, std::memory_order_release);
        } else {
            state->receivedTimeout.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
            state->receivedClosed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
        }
    } else if (kind == AwaitKind::kRecvBatch) {
        auto result = co_await channel->recvBatch(2).timeout(1h);
        if (result.has_value()) {
            int sum = 0;
            for (int value : *result) {
                sum += value;
            }
            state->operationSucceeded.store(true, std::memory_order_release);
            state->valueCount.store(result->size(), std::memory_order_release);
            state->valueSum.store(sum, std::memory_order_release);
        } else {
            state->receivedTimeout.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
            state->receivedClosed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
        }
    } else {
        constexpr int kFirstSentinel = -7001;
        constexpr int kSecondSentinel = -7002;
        std::array<int, 2> output{kFirstSentinel, kSecondSentinel};
        auto result = co_await channel->recvBatchTo(std::span<int>(output)).timeout(1h);
        if (result.has_value()) {
            const size_t count = *result;
            int sum = 0;
            for (size_t index = 0; index < count && index < output.size(); ++index) {
                sum += output[index];
            }
            state->callerBufferContractOk.store(
                count == 1 && output[0] == 73 &&
                    output[1] == kSecondSentinel,
                std::memory_order_release);
            state->operationSucceeded.store(true, std::memory_order_release);
            state->valueCount.store(count, std::memory_order_release);
            state->valueSum.store(sum, std::memory_order_release);
        } else {
            state->callerBufferContractOk.store(
                output[0] == kFirstSentinel &&
                    output[1] == kSecondSentinel,
                std::memory_order_release);
            state->receivedTimeout.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
            state->receivedClosed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
        }
    }
    [[maybe_unused]] const int previousResumeCount =
        state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    state->completed.store(true, std::memory_order_release);
    co_return;
}

Task<void> awaitWithoutTimeout(galay::spsc::BoundedChannel<int>* channel,
                               AwaitKind kind,
                               AwaitState* state)
{
    if (kind == AwaitKind::kSend) {
        auto result = co_await channel->send(73);
        state->operationSucceeded.store(result.has_value(), std::memory_order_release);
        if (!result.has_value()) {
            state->receivedNotReady.store(
                IOError::contains(result.error().code(), kNotReady),
                std::memory_order_release);
            state->receivedOutOfMemory.store(
                IOError::contains(result.error().code(), kOutOfMemory),
                std::memory_order_release);
        }
    } else if (kind == AwaitKind::kRecv) {
        auto result = co_await channel->recv();
        if (result.has_value()) {
            state->operationSucceeded.store(true, std::memory_order_release);
            state->valueCount.store(1, std::memory_order_release);
            state->valueSum.store(*result, std::memory_order_release);
        } else {
            state->receivedNotReady.store(
                IOError::contains(result.error().code(), kNotReady),
                std::memory_order_release);
            state->receivedOutOfMemory.store(
                IOError::contains(result.error().code(), kOutOfMemory),
                std::memory_order_release);
        }
    } else if (kind == AwaitKind::kRecvBatch) {
        auto result = co_await channel->recvBatch(2);
        if (result.has_value()) {
            int sum = 0;
            for (int value : *result) {
                sum += value;
            }
            state->operationSucceeded.store(true, std::memory_order_release);
            state->valueCount.store(result->size(), std::memory_order_release);
            state->valueSum.store(sum, std::memory_order_release);
        } else {
            state->receivedNotReady.store(
                IOError::contains(result.error().code(), kNotReady),
                std::memory_order_release);
            state->receivedOutOfMemory.store(
                IOError::contains(result.error().code(), kOutOfMemory),
                std::memory_order_release);
        }
    } else {
        constexpr int kFirstSentinel = -7001;
        constexpr int kSecondSentinel = -7002;
        std::array<int, 2> output{kFirstSentinel, kSecondSentinel};
        const size_t allocationsBefore =
            g_allocationCount.load(std::memory_order_relaxed);
        auto result = co_await channel->recvBatchTo(std::span<int>(output));
        const size_t allocationsAfter =
            g_allocationCount.load(std::memory_order_relaxed);
        state->awaitAllocationCount.store(
            allocationsAfter - allocationsBefore,
            std::memory_order_release);
        if (result.has_value()) {
            const size_t count = *result;
            int sum = 0;
            for (size_t index = 0; index < count && index < output.size(); ++index) {
                sum += output[index];
            }
            state->callerBufferContractOk.store(
                count == 1 && output[0] == 73 &&
                    output[1] == kSecondSentinel,
                std::memory_order_release);
            state->operationSucceeded.store(true, std::memory_order_release);
            state->valueCount.store(count, std::memory_order_release);
            state->valueSum.store(sum, std::memory_order_release);
        } else {
            state->callerBufferContractOk.store(
                output[0] == kFirstSentinel &&
                    output[1] == kSecondSentinel,
                std::memory_order_release);
            state->receivedNotReady.store(
                IOError::contains(result.error().code(), kNotReady),
                std::memory_order_release);
            state->receivedOutOfMemory.store(
                IOError::contains(result.error().code(), kOutOfMemory),
                std::memory_order_release);
        }
    }
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    state->completed.store(true, std::memory_order_release);
    co_return;
}

Task<void> receiveBlockingMove(
    galay::spsc::BoundedChannel<BlockingMove>* channel,
    BlockingRecvState* state)
{
    auto result = co_await channel->recv();
    if (result.has_value()) {
        state->succeeded = true;
        state->value = result->value;
    } else {
        state->receivedClosed = IOError::contains(result.error().code(), kClosed);
    }
    state->completed.store(true, std::memory_order_release);
    co_return;
}

Task<void> receiveClosedAndDestroy(
    std::unique_ptr<galay::spsc::BoundedChannel<int>>* owner,
    AwaitState* state)
{
    auto result = co_await (*owner)->recv();
    state->receivedClosed.store(
        !result.has_value() && IOError::contains(result.error().code(), kClosed),
        std::memory_order_release);
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    state->completed.store(true, std::memory_order_release);
    owner->reset();
    co_return;
}

bool hasExpectedSuccess(const AwaitState& state, AwaitKind kind) noexcept
{
    if (!state.operationSucceeded.load(std::memory_order_acquire) ||
        state.receivedTimeout.load(std::memory_order_acquire) ||
        state.receivedClosed.load(std::memory_order_acquire) ||
        !state.callerBufferContractOk.load(std::memory_order_acquire)) {
        return false;
    }
    if (kind == AwaitKind::kSend) {
        return state.valueCount.load(std::memory_order_acquire) == 0;
    }
    return state.valueCount.load(std::memory_order_acquire) == 1 &&
        state.valueSum.load(std::memory_order_acquire) == 73;
}

bool hasExpectedTimeout(const AwaitState& state) noexcept
{
    return !state.operationSucceeded.load(std::memory_order_acquire) &&
        state.receivedTimeout.load(std::memory_order_acquire) &&
        !state.receivedClosed.load(std::memory_order_acquire) &&
        state.callerBufferContractOk.load(std::memory_order_acquire) &&
        state.valueCount.load(std::memory_order_acquire) == 0;
}

bool hasExpectedClose(const AwaitState& state) noexcept
{
    return !state.operationSucceeded.load(std::memory_order_acquire) &&
        !state.receivedTimeout.load(std::memory_order_acquire) &&
        state.receivedClosed.load(std::memory_order_acquire) &&
        state.callerBufferContractOk.load(std::memory_order_acquire) &&
        state.valueCount.load(std::memory_order_acquire) == 0;
}

bool ringMatches(galay::spsc::BoundedChannel<int>& channel,
                 AwaitKind kind,
                 bool operationWon,
                 bool counterpartRan)
{
    if (kind == AwaitKind::kSend) {
        auto first = channel.tryRecv();
        auto second = channel.tryRecv();
        auto third = channel.tryRecv();
        if (operationWon) {
            return first.has_value() && *first == 12 && second.has_value() &&
                *second == 73 && !third.has_value();
        }
        if (!counterpartRan) {
            return first.has_value() && *first == 11 && second.has_value() &&
                *second == 12 && !third.has_value();
        }
        return first.has_value() && *first == 12 && !second.has_value() &&
            !third.has_value();
    }

    auto retained = channel.tryRecv();
    auto extra = channel.tryRecv();
    if (operationWon) {
        return !retained.has_value() && !extra.has_value();
    }
    if (!counterpartRan) {
        return !retained.has_value() && !extra.has_value();
    }
    return retained.has_value() && *retained == 73 && !extra.has_value();
}

bool runAddTimerFailure(AwaitKind kind, bool completeOperation)
{
    galay::spsc::BoundedChannel<int> channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    ImmediateRaceScheduler scheduler(&channel, kind, completeOperation);
    AwaitState state;

    auto task = awaitWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool resultCorrect = completeOperation
        ? hasExpectedSuccess(state, kind)
        : hasExpectedTimeout(state);
    const bool ringCorrect =
        ringMatches(channel, kind, completeOperation, completeOperation);
    // timeout 在外层 DeferredWaker arm 前获胜时由当前栈同步继续，不会额外入队。
    const int expectedScheduleCalls = completeOperation ? 1 : 0;

    return started && state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 && resultCorrect &&
        ringCorrect && scheduler.addTimerCalls() == 1 &&
        scheduler.scheduleCalls() == expectedScheduleCalls &&
        (!completeOperation || scheduler.actionSucceeded()) &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runTimedOutWaiterDoesNotBlockNext(AwaitKind kind)
{
    galay::spsc::BoundedChannel<int> channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    AwaitState timedOutState;
    AwaitState liveState;

    auto timedOutTask = awaitWithTimeout(&channel, kind, &timedOutState);
    TaskRef timedOutKeeper = detail::TaskAccess::taskRef(timedOutTask);
    TaskState* timedOutTaskState = timedOutKeeper.state();
    if (timedOutTaskState == nullptr) {
        return false;
    }
    TaskRef timedOutScheduled = detail::TaskAccess::detachTask(std::move(timedOutTask));
    if (!scheduler.scheduleImmediately(std::move(timedOutScheduled)) ||
        !scheduler.fireTimer() || !scheduler.hasSingleReadyTask() ||
        !scheduler.runOne() || !hasExpectedTimeout(timedOutState)) {
        scheduler.releaseRetainedState();
        return false;
    }

    auto liveTask = awaitWithTimeout(&channel, kind, &liveState);
    TaskRef liveKeeper = detail::TaskAccess::taskRef(liveTask);
    TaskState* liveTaskState = liveKeeper.state();
    if (liveTaskState == nullptr) {
        scheduler.releaseRetainedState();
        return false;
    }
    TaskRef liveScheduled = detail::TaskAccess::detachTask(std::move(liveTask));
    const bool liveStarted = scheduler.scheduleImmediately(std::move(liveScheduled));
    const bool counterpartCompleted = completeWaitingOperation(channel, kind);
    const bool oneLiveWake = scheduler.hasSingleReadyTask();
    const bool liveResumed = scheduler.runOne();
    const bool liveCompleted = hasExpectedSuccess(liveState, kind) &&
        liveState.resumeCount.load(std::memory_order_acquire) == 1;
    const bool ringCorrect = ringMatches(channel, kind, true, true);

    scheduler.releaseRetainedState();
    return liveStarted && counterpartCompleted && oneLiveWake && liveResumed &&
        liveCompleted && ringCorrect &&
        timedOutTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        liveTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runTimeoutWhilePeerHoldsWaiterLease(AwaitKind kind)
{
    galay::spsc::BoundedChannel<int> channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    AwaitState timedOutState;

    auto timedOutTask = awaitWithTimeout(&channel, kind, &timedOutState);
    TaskRef timedOutKeeper = detail::TaskAccess::taskRef(timedOutTask);
    TaskRef timedOutScheduled = detail::TaskAccess::detachTask(std::move(timedOutTask));
    if (!scheduler.scheduleImmediately(std::move(timedOutScheduled))) {
        return false;
    }

    bounded_waiter_lease_test::reset();
    std::atomic<bool> counterpartCompleted{false};
    std::thread counterpart([&]() {
        counterpartCompleted.store(completeWaitingOperation(channel, kind),
                                   std::memory_order_release);
    });

    const bool dequeued = waitForFlag(bounded_waiter_lease_test::gate.dequeued);
    const bool timerFired = dequeued && scheduler.fireTimer();
    const bool oneTimerWake = timerFired && scheduler.hasSingleReadyTask();
    std::atomic<bool> resumed{false};
    std::thread resumeThread;
    if (oneTimerWake) {
        resumeThread = std::thread([&]() {
            resumed.store(scheduler.runOne(), std::memory_order_release);
        });
    }

    const bool reclaimWaitedForLease = oneTimerWake &&
        waitForFlag(bounded_waiter_lease_test::gate.reclaiming);
    bounded_waiter_lease_test::release();
    counterpart.join();
    if (resumeThread.joinable()) {
        resumeThread.join();
    }

    const bool timeoutCompleted = resumed.load(std::memory_order_acquire) &&
        hasExpectedTimeout(timedOutState) &&
        timedOutState.resumeCount.load(std::memory_order_acquire) == 1;

    bool normalized = false;
    if (kind == AwaitKind::kSend) {
        normalized = channel.trySend(99) && channel.full();
    } else {
        auto retained = channel.tryRecv();
        auto extra = channel.tryRecv();
        normalized = retained.has_value() && *retained == 73 && !extra.has_value();
    }

    AwaitState liveState;
    auto liveTask = awaitWithoutTimeout(&channel, kind, &liveState);
    TaskRef liveKeeper = detail::TaskAccess::taskRef(liveTask);
    TaskRef liveScheduled = detail::TaskAccess::detachTask(std::move(liveTask));
    const bool liveStarted = normalized &&
        scheduler.scheduleImmediately(std::move(liveScheduled));
    const bool liveSuspended = liveStarted &&
        !liveState.completed.load(std::memory_order_acquire);
    bool liveCounterpartCompleted = false;
    if (liveSuspended && kind == AwaitKind::kSend) {
        auto released = channel.tryRecv();
        liveCounterpartCompleted = released.has_value() && *released == 12;
    } else if (liveSuspended) {
        liveCounterpartCompleted = channel.trySend(73);
    }
    const bool oneLiveWake = liveCounterpartCompleted && scheduler.hasSingleReadyTask();
    const bool liveResumed = oneLiveWake && scheduler.runOne();
    const bool liveCompleted = liveResumed && hasExpectedSuccess(liveState, kind) &&
        liveState.resumeCount.load(std::memory_order_acquire) == 1;

    scheduler.releaseRetainedState();
    const bool passed = dequeued && timerFired && oneTimerWake && reclaimWaitedForLease &&
        counterpartCompleted.load(std::memory_order_acquire) && timeoutCompleted &&
        normalized && liveSuspended && liveCounterpartCompleted && oneLiveWake &&
        liveCompleted;
    if (!passed) {
        std::cerr << "[T160] pinned lease detail kind=" << awaitKindName(kind)
                  << " dequeued=" << dequeued
                  << " timer=" << timerFired
                  << " timer_wake=" << oneTimerWake
                  << " reclaim_wait=" << reclaimWaitedForLease
                  << " counterpart="
                  << counterpartCompleted.load(std::memory_order_acquire)
                  << " timeout_done=" << timeoutCompleted
                  << " normalized=" << normalized
                  << " live_suspended=" << liveSuspended
                  << " live_counterpart=" << liveCounterpartCompleted
                  << " live_wake=" << oneLiveWake
                  << " live_done=" << liveCompleted << '\n';
    }
    return passed;
}

bool runWaiterPathUsesNoAllocation(AwaitKind kind)
{
    galay::spsc::BoundedChannel<int> channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    AwaitState state;

    auto task = awaitWithoutTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const size_t allocationsBefore =
        g_allocationCount.load(std::memory_order_relaxed);
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const size_t allocationsAfter =
        g_allocationCount.load(std::memory_order_relaxed);
    const bool suspended = started && !state.completed.load(std::memory_order_acquire);

    const bool counterpartCompleted = completeWaitingOperation(channel, kind);
    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    const bool completed = hasExpectedSuccess(state, kind);
    scheduler.releaseRetainedState();
    return suspended && counterpartCompleted && oneWake && resumed && completed &&
        allocationsAfter == allocationsBefore;
}

bool runConstructionOomIsRecoverable(AwaitKind kind)
{
    g_failAllocation.store(true, std::memory_order_release);
    galay::spsc::BoundedChannel<int> channel(2);
    g_failAllocation.store(false, std::memory_order_release);
    if (channel.error() != galay::spsc::RingError::kAllocationFailed ||
        channel.capacity() != 0 || channel.trySend(73)) {
        return false;
    }

    QueuedRaceScheduler scheduler;
    AwaitState state;
    auto task = awaitWithoutTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool completed = started &&
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedOutOfMemory.load(std::memory_order_acquire) &&
        !state.receivedNotReady.load(std::memory_order_acquire) &&
        !state.operationSucceeded.load(std::memory_order_acquire) &&
        !scheduler.hasSingleReadyTask();
    scheduler.releaseRetainedState();
    return completed && taskState != nullptr &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runDynamicCapacityByteBound()
{
    constexpr size_t kCursorHalf = size_t{1} <<
        (std::numeric_limits<size_t>::digits - 1U);
    galay::spsc::BoundedChannel<LargeNothrowValue> channel(kCursorHalf);
    return channel.error() == galay::spsc::RingError::kCapacityTooLarge &&
        channel.capacity() == 0;
}

bool runInvalidCapacityErrorPropagation()
{
    galay::spsc::BoundedChannel<int> channel(
        std::numeric_limits<size_t>::max());
    if (channel.error() != galay::spsc::RingError::kCapacityTooLarge ||
        channel.capacity() != 0) {
        return false;
    }

    const auto isParamInvalid = [](const auto& result) {
        return !result.has_value() && IOError::contains(
            result.error().code(), kParamInvalid);
    };

    auto send = channel.send(73);
    if (!send.await_ready() || !isParamInvalid(send.await_resume())) {
        return false;
    }

    auto recv = channel.recv();
    if (!recv.await_ready() || !isParamInvalid(recv.await_resume())) {
        return false;
    }

    auto batch = channel.recvBatch(1);
    if (!batch.await_ready() || !isParamInvalid(batch.await_resume())) {
        return false;
    }

    std::array<int, 1> output{};
    auto batchTo = channel.recvBatchTo(std::span<int>(output));
    return batchTo.await_ready() && isParamInvalid(batchTo.await_resume());
}

bool runDiagnosticSizeAcrossCursorWrap()
{
    galay::spsc::BoundedChannel<int> channel(4);
    if (channel.error() != galay::spsc::RingError::kNone) {
        return false;
    }
    channel.m_head.store(std::numeric_limits<size_t>::max() - 1,
                         std::memory_order_relaxed);
    channel.m_tail.store(1, std::memory_order_relaxed);
    return channel.size() == 3 && !channel.empty() && !channel.full();
}

bool runRecvBatchToReadyPathUsesNoAllocation()
{
    galay::spsc::BoundedChannel<int> channel(2);
    if (!channel.trySend(73)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    AwaitState state;

    auto task = awaitWithoutTimeout(&channel, AwaitKind::kRecvBatchTo, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const size_t allocationsBefore =
        g_allocationCount.load(std::memory_order_relaxed);
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const size_t allocationsAfter =
        g_allocationCount.load(std::memory_order_relaxed);
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        hasExpectedSuccess(state, AwaitKind::kRecvBatchTo);
    scheduler.releaseRetainedState();
    return started && completed && allocationsAfter == allocationsBefore &&
        state.awaitAllocationCount.load(std::memory_order_acquire) == 0;
}

bool runSameSideDoubleRegistrationRejected()
{
    galay::spsc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    AwaitState firstState;
    AwaitState secondState;

    auto firstTask = awaitWithoutTimeout(&channel, AwaitKind::kRecv, &firstState);
    TaskRef firstKeeper = detail::TaskAccess::taskRef(firstTask);
    TaskRef firstScheduled = detail::TaskAccess::detachTask(std::move(firstTask));
    if (!scheduler.scheduleImmediately(std::move(firstScheduled)) ||
        firstState.completed.load(std::memory_order_acquire)) {
        scheduler.releaseRetainedState();
        return false;
    }

    auto secondTask =
        awaitWithoutTimeout(&channel, AwaitKind::kRecvBatchTo, &secondState);
    TaskRef secondKeeper = detail::TaskAccess::taskRef(secondTask);
    TaskRef secondScheduled = detail::TaskAccess::detachTask(std::move(secondTask));
    const bool secondStarted = scheduler.scheduleImmediately(std::move(secondScheduled));
    const bool secondRejected = secondState.completed.load(std::memory_order_acquire) &&
        secondState.receivedNotReady.load(std::memory_order_acquire) &&
        secondState.callerBufferContractOk.load(std::memory_order_acquire) &&
        secondState.resumeCount.load(std::memory_order_acquire) == 1;

    const bool sent = channel.trySend(73);
    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool firstResumed = scheduler.runOne();
    const bool firstCompleted = hasExpectedSuccess(firstState, AwaitKind::kRecv) &&
        firstState.resumeCount.load(std::memory_order_acquire) == 1;
    scheduler.releaseRetainedState();
    return secondStarted && secondRejected && sent && oneWake && firstResumed &&
        firstCompleted;
}

bool runRetryArrivesBeforeTimeoutAbort(AwaitKind kind)
{
    galay::spsc::BoundedChannel<int> channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    AwaitState state;

    auto task = awaitWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    std::atomic<bool> started{false};

    bounded_timeout_abort_test::reset();
    std::thread owner([&]() {
        started.store(
            scheduler.scheduleImmediately(std::move(scheduled)),
            std::memory_order_release);
    });

    const bool ownerReachedAbort =
        waitForFlag(bounded_timeout_abort_test::gate.arrived);
    const bool counterpartCompleted =
        ownerReachedAbort && completeWaitingOperation(channel, kind);
    bounded_timeout_abort_test::release();
    owner.join();

    const bool completedBeforeCleanup =
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        hasExpectedSuccess(state, kind) &&
        ringMatches(channel, kind, true, true);

    if (!state.completed.load(std::memory_order_acquire)) {
        if (scheduler.fireTimer() && scheduler.hasSingleReadyTask()) {
            [[maybe_unused]] const bool resumed = scheduler.runOne();
        }
    }
    scheduler.releaseRetainedState();
    return started.load(std::memory_order_acquire) && ownerReachedAbort &&
        counterpartCompleted && completedBeforeCleanup &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runCloseWaitsForStartedSendPublication()
{
    using Channel = galay::spsc::BoundedChannel<BlockingMove>;
    Channel channel(2);
    QueuedRaceScheduler scheduler;
    BlockingRecvState recvState;

    auto recvTask = receiveBlockingMove(&channel, &recvState);
    TaskRef recvKeeper = detail::TaskAccess::taskRef(recvTask);
    TaskState* recvTaskState = recvKeeper.state();
    if (recvTaskState == nullptr) {
        return false;
    }
    TaskRef recvScheduled = detail::TaskAccess::detachTask(std::move(recvTask));
    if (!scheduler.scheduleImmediately(std::move(recvScheduled))) {
        return false;
    }

    BlockingMoveControl control;
    std::atomic<bool> sendSucceeded{false};
    std::thread producer([&]() {
        BlockingMove value(91, &control);
        sendSucceeded.store(channel.trySend(std::move(value)),
                            std::memory_order_release);
    });
    const bool moveStarted = waitForFlag(control.moveStarted);
    if (moveStarted) {
        channel.close();
    }
    const bool closeDeferred = moveStarted && !scheduler.hasSingleReadyTask();
    control.allowMove.store(true, std::memory_order_release);
    producer.join();

    const bool oneValueWake = scheduler.hasSingleReadyTask();
    const bool recvResumed = scheduler.runOne();
    const bool recvCompleted = recvState.completed.load(std::memory_order_acquire) &&
        recvState.succeeded && !recvState.receivedClosed && recvState.value == 91;
    const bool drained = channel.empty();

    scheduler.releaseRetainedState();
    return moveStarted && closeDeferred &&
        sendSucceeded.load(std::memory_order_acquire) && oneValueWake &&
        recvResumed && recvCompleted && drained && channel.isClosed() &&
        recvTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runSynchronousCloseWakeCanDestroyChannel()
{
    auto channel = std::make_unique<galay::spsc::BoundedChannel<int>>(2);
    ImmediateRaceScheduler scheduler(nullptr, AwaitKind::kRecv, false);
    AwaitState state;

    auto task = receiveClosedAndDestroy(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    if (!scheduler.scheduleImmediately(std::move(scheduled))) {
        return false;
    }

    auto* rawChannel = channel.get();
    rawChannel->close();
    return channel == nullptr && state.completed.load(std::memory_order_acquire) &&
        state.receivedClosed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        scheduler.scheduleCalls() == 1 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runOperationFirst(AwaitKind kind)
{
    galay::spsc::BoundedChannel<int> channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    AwaitState state;

    auto task = awaitWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool operationCompleted = completeWaitingOperation(channel, kind);
    const bool oneOperationWake = scheduler.hasSingleReadyTask();
    const bool noDuplicateWake = scheduler.resumeWithTimerInDequeueGap();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        hasExpectedSuccess(state, kind);
    const bool ringCorrect = ringMatches(channel, kind, true, true);

    scheduler.releaseRetainedState();
    return started && operationCompleted && oneOperationWake && noDuplicateWake &&
        completed && ringCorrect &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runTimerFirst(AwaitKind kind)
{
    galay::spsc::BoundedChannel<int> channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    AwaitState state;

    auto task = awaitWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool timerFired = scheduler.fireTimer();
    const bool oneTimerWake = scheduler.hasSingleReadyTask();
    const bool counterpartCompleted = completeWaitingOperation(channel, kind);
    const bool stillOneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    const bool timedOut = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        hasExpectedTimeout(state);
    const bool ringCorrect = ringMatches(channel, kind, false, true);

    scheduler.releaseRetainedState();
    return started && timerFired && oneTimerWake && counterpartCompleted &&
        stillOneWake && resumed && timedOut && ringCorrect &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runCloseTimerArbitration(AwaitKind kind, bool closeFirst)
{
    galay::spsc::BoundedChannel<int> channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    AwaitState state;

    auto task = awaitWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    bool oneWake = false;
    bool noDuplicateWake = false;
    bool resumed = false;
    if (closeFirst) {
        channel.close();
        oneWake = scheduler.hasSingleReadyTask();
        noDuplicateWake = scheduler.resumeWithTimerInDequeueGap();
        resumed = true;
    } else {
        const bool timerFired = scheduler.fireTimer();
        oneWake = timerFired && scheduler.hasSingleReadyTask();
        channel.close();
        noDuplicateWake = scheduler.hasSingleReadyTask();
        resumed = scheduler.runOne();
    }

    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        (closeFirst ? hasExpectedClose(state) : hasExpectedTimeout(state));
    const bool ringCorrect = ringMatches(channel, kind, false, false);

    scheduler.releaseRetainedState();
    return started && oneWake && noDuplicateWake && resumed && completed &&
        ringCorrect && taskState->m_refs.load(std::memory_order_acquire) == 1;
}

}  // namespace

int main()
{
    if (!runDynamicCapacityByteBound()) {
        std::cerr << "[T160] dynamic capacity ignored slot byte-size bound\n";
        return 1;
    }
    if (!runInvalidCapacityErrorPropagation()) {
        std::cerr << "[T160] invalid capacity error was not propagated\n";
        return 1;
    }
    if (!runDiagnosticSizeAcrossCursorWrap()) {
        std::cerr << "[T160] diagnostic size failed across cursor wrap\n";
        return 1;
    }
    if (!runStaticCapacityChannel()) {
        std::cerr << "[T160] static-capacity bounded channel failed\n";
        return 1;
    }

    for (AwaitKind kind : kAwaitKinds) {
        if (!runConstructionOomIsRecoverable(kind)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " construction OOM was not propagated\n";
            return 1;
        }
        if (!runWaiterPathUsesNoAllocation(kind)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " waiter suspension performed a global allocation\n";
            return 1;
        }
        if (!runRetryArrivesBeforeTimeoutAbort(kind)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " retry between final check and timeout abort was lost\n";
            return 1;
        }
        if (!runPeerCompletionBeforeArmed(kind)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " peer completion escaped the await_suspend arming window\n";
            return 1;
        }
        if (!runTimedOutWaiterDoesNotBlockNext(kind)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " timeout tombstone blocked the next live waiter\n";
            return 1;
        }
        if (!runTimeoutWhilePeerHoldsWaiterLease(kind)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " pinned waiter was reused before peer release\n";
            return 1;
        }
        if (!runAddTimerFailure(kind, true)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " addTimer=false operation-first failed\n";
            return 1;
        }
        if (!runAddTimerFailure(kind, false)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " addTimer=false timeout-first failed\n";
            return 1;
        }
        if (!runOperationFirst(kind)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " operation-first dequeue-gap failed\n";
            return 1;
        }
        if (!runTimerFirst(kind)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " timer-first retention/non-send failed\n";
            return 1;
        }
        if (!runCloseTimerArbitration(kind, true)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " close-first arbitration failed\n";
            return 1;
        }
        if (!runCloseTimerArbitration(kind, false)) {
            std::cerr << "[T160] " << awaitKindName(kind)
                      << " timer-first close arbitration failed\n";
            return 1;
        }
    }

    if (!runRecvBatchToReadyPathUsesNoAllocation()) {
        std::cerr << "[T160] recvBatchTo ready path performed a global allocation\n";
        return 1;
    }

    if (!runSameSideDoubleRegistrationRejected()) {
        std::cerr << "[T160] same-side double waiter registration was not rejected\n";
        return 1;
    }

    if (!runCloseWaitsForStartedSendPublication()) {
        std::cerr << "[T160] close overtook an already-started SPSC send\n";
        return 1;
    }
    if (!runSynchronousCloseWakeCanDestroyChannel()) {
        std::cerr << "[T160] synchronous close wake lifetime failed\n";
        return 1;
    }

    std::cout << "T160-SpscBoundedTimeoutRace PASS\n";
    return 0;
}
