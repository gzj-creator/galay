/**
 * @file t157_spsc_timeout_race.cc
 * @brief 验证 SPSC unbounded 三种接收等待体只由 producer 或 timeout 完成一次。
 */

#include <galay/cpp/galay-kernel/core/scheduler.hpp>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/timeout.hpp>
#include <galay/cpp/galay-kernel/core/wait_registration.h>
#include <galay/cpp/galay-kernel/core/waker.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace unbounded_registering_test {
void pauseClaimed() noexcept;
void pause() noexcept;
void pauseWaking() noexcept;
void observeReclaimWait() noexcept;
}

#define GALAY_SPSC_UNBOUNDED_CLAIMED_TEST_POINT() \
    ::unbounded_registering_test::pauseClaimed()
#define GALAY_SPSC_UNBOUNDED_REGISTERING_TEST_POINT() \
    ::unbounded_registering_test::pause()
#define GALAY_SPSC_UNBOUNDED_WAKING_TEST_POINT() \
    ::unbounded_registering_test::pauseWaking()
#define GALAY_SPSC_UNBOUNDED_RECLAIM_WAIT_TEST_POINT() \
    ::unbounded_registering_test::observeReclaimWait()
#define private public
#include <galay/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h>
#undef private
#undef GALAY_SPSC_UNBOUNDED_CLAIMED_TEST_POINT
#undef GALAY_SPSC_UNBOUNDED_RECLAIM_WAIT_TEST_POINT
#undef GALAY_SPSC_UNBOUNDED_WAKING_TEST_POINT
#undef GALAY_SPSC_UNBOUNDED_REGISTERING_TEST_POINT

namespace unbounded_registering_test {

struct Gate
{
    std::atomic<bool> enabled{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> proceed{false};
};

Gate g_gate;
Gate g_claimedGate;
Gate g_wakingGate;
std::atomic<bool> g_reclaimWaitObserved{false};

void pauseClaimed() noexcept
{
    if (!g_claimedGate.enabled.load(std::memory_order_acquire)) {
        return;
    }
    g_claimedGate.entered.store(true, std::memory_order_release);
    while (!g_claimedGate.proceed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void pause() noexcept
{
    if (!g_gate.enabled.load(std::memory_order_acquire)) {
        return;
    }
    g_gate.entered.store(true, std::memory_order_release);
    while (!g_gate.proceed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void pauseWaking() noexcept
{
    if (!g_wakingGate.enabled.load(std::memory_order_acquire)) {
        return;
    }
    g_wakingGate.entered.store(true, std::memory_order_release);
    while (!g_wakingGate.proceed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void observeReclaimWait() noexcept
{
    g_reclaimWaitObserved.store(true, std::memory_order_release);
}

} // namespace unbounded_registering_test

namespace {

using namespace galay::kernel;
using namespace std::chrono_literals;

enum class ReceiveKind {
    kRecv,
    kRecvBatch,
    kRecvBatchTo,
    kRecvBatched,
};

constexpr std::array<ReceiveKind, 4> kReceiveKinds{
    ReceiveKind::kRecv,
    ReceiveKind::kRecvBatch,
    ReceiveKind::kRecvBatchTo,
    ReceiveKind::kRecvBatched,
};

const char* receiveKindName(ReceiveKind kind) noexcept
{
    switch (kind) {
    case ReceiveKind::kRecv:
        return "recv";
    case ReceiveKind::kRecvBatch:
        return "recvBatch";
    case ReceiveKind::kRecvBatchTo:
        return "recvBatchTo";
    case ReceiveKind::kRecvBatched:
        return "recvBatched";
    }
    return "unknown";
}

size_t expectedCount(ReceiveKind kind) noexcept
{
    return kind == ReceiveKind::kRecv ? 1 : 2;
}

struct ReceiveState
{
    std::atomic<size_t> valueCount{0};
    std::array<int, 2> batchOutput{-1, -1};
    std::atomic<int> valueSum{0};
    std::atomic<int> resumeCount{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> receivedTimeout{false};
    std::atomic<bool> receivedNotReady{false};
};

bool sendValues(galay::spsc::UnboundedChannel<int>& channel, ReceiveKind kind)
{
    if (kind == ReceiveKind::kRecv) {
        return channel.send(73);
    }
    std::vector<int> values{73, 74};
    return channel.sendBatch(std::move(values));
}

class ImmediateRaceScheduler final : public Scheduler
{
public:
    ImmediateRaceScheduler(galay::spsc::UnboundedChannel<int>* channel,
                           ReceiveKind kind,
                           bool publishValues) noexcept
        : m_channel(channel), m_kind(kind), m_publishValues(publishValues)
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
        if (m_publishValues) {
            m_sendSucceeded.store(
                sendValues(*m_channel, m_kind), std::memory_order_release);
        }
        // 强制 WithTimeout 走 timeoutNow()，覆盖 inner waiter 发布后的同步完成窗口。
        return false;
    }

    SchedulerType type() override { return kComputeScheduler; }

    int scheduleCalls() const noexcept
    {
        return m_scheduleCalls.load(std::memory_order_acquire);
    }

    int addTimerCalls() const noexcept
    {
        return m_addTimerCalls.load(std::memory_order_acquire);
    }

    bool sendSucceeded() const noexcept
    {
        return m_sendSucceeded.load(std::memory_order_acquire);
    }

private:
    galay::spsc::UnboundedChannel<int>* m_channel;
    const ReceiveKind m_kind;
    const bool m_publishValues;
    std::atomic<int> m_scheduleCalls{0};
    std::atomic<int> m_addTimerCalls{0};
    std::atomic<bool> m_sendSucceeded{false};
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

    SchedulerType type() override { return kComputeScheduler; }

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

Task<void> receiveWithTimeout(galay::spsc::UnboundedChannel<int>* channel,
                              ReceiveKind kind,
                              ReceiveState* state)
{
    if (kind == ReceiveKind::kRecv) {
        auto result = co_await channel->recv().timeout(1h);
        if (result.has_value()) {
            state->valueCount.store(1, std::memory_order_release);
            state->valueSum.store(*result, std::memory_order_release);
        } else {
            state->receivedTimeout.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
            state->receivedNotReady.store(
                IOError::contains(result.error().code(), kNotReady),
                std::memory_order_release);
        }
    } else if (kind == ReceiveKind::kRecvBatch) {
        auto result = co_await channel->recvBatch(2).timeout(1h);
        if (result.has_value()) {
            int sum = 0;
            for (int value : *result) {
                sum += value;
            }
            state->valueCount.store(result->size(), std::memory_order_release);
            state->valueSum.store(sum, std::memory_order_release);
        } else {
            state->receivedTimeout.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
            state->receivedNotReady.store(
                IOError::contains(result.error().code(), kNotReady),
                std::memory_order_release);
        }
    } else if (kind == ReceiveKind::kRecvBatchTo) {
        auto result = co_await channel->recvBatchTo(
            std::span<int>(state->batchOutput)).timeout(1h);
        if (result.has_value()) {
            int sum = 0;
            for (size_t index = 0; index < *result; ++index) {
                sum += state->batchOutput[index];
            }
            state->valueCount.store(*result, std::memory_order_release);
            state->valueSum.store(sum, std::memory_order_release);
        } else {
            state->receivedTimeout.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
            state->receivedNotReady.store(
                IOError::contains(result.error().code(), kNotReady),
                std::memory_order_release);
        }
    } else {
        auto result = co_await channel->recvBatched(2).timeout(1h);
        if (result.has_value()) {
            int sum = 0;
            for (int value : *result) {
                sum += value;
            }
            state->valueCount.store(result->size(), std::memory_order_release);
            state->valueSum.store(sum, std::memory_order_release);
        } else {
            state->receivedTimeout.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
            state->receivedNotReady.store(
                IOError::contains(result.error().code(), kNotReady),
                std::memory_order_release);
        }
    }
    [[maybe_unused]] const int previousResumeCount =
        state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    state->completed.store(true, std::memory_order_release);
    co_return;
}

bool receivedExpectedValues(const ReceiveState& state, ReceiveKind kind) noexcept
{
    const size_t count = expectedCount(kind);
    const int sum = kind == ReceiveKind::kRecv ? 73 : 147;
    return state.valueCount.load(std::memory_order_acquire) == count &&
        state.valueSum.load(std::memory_order_acquire) == sum &&
        (kind != ReceiveKind::kRecvBatchTo ||
         state.batchOutput == std::array<int, 2>{73, 74}) &&
        !state.receivedTimeout.load(std::memory_order_acquire);
}

bool callerOwnedBufferUnchanged(const ReceiveState& state,
                                ReceiveKind kind) noexcept
{
    return kind != ReceiveKind::kRecvBatchTo ||
        state.batchOutput == std::array<int, 2>{-1, -1};
}

bool drainExpectedValues(galay::spsc::UnboundedChannel<int>& channel,
                         ReceiveKind kind)
{
    std::array<int, 2> values{-1, -1};
    const size_t count = channel.tryRecvBatch(
        std::span<int>(values).first(expectedCount(kind)));
    if (count != expectedCount(kind)) {
        return false;
    }
    int sum = 0;
    for (size_t index = 0; index < count; ++index) {
        sum += values[index];
    }
    const int expectedSum = kind == ReceiveKind::kRecv ? 73 : 147;
    return sum == expectedSum && channel.empty();
}

bool runImmediateCompletion(ReceiveKind kind, bool publishValues)
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    ImmediateRaceScheduler scheduler(&channel, kind, publishValues);
    ReceiveState state;

    auto task = receiveWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool correctResult = publishValues
        ? receivedExpectedValues(state, kind) && scheduler.sendSucceeded()
        : state.valueCount.load(std::memory_order_acquire) == 0 &&
            state.receivedTimeout.load(std::memory_order_acquire) &&
            callerOwnedBufferUnchanged(state, kind);
    const int expectedScheduleCalls = publishValues ? 1 : 0;

    return started && state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 && correctResult &&
        scheduler.addTimerCalls() == 1 &&
        scheduler.scheduleCalls() == expectedScheduleCalls &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool runProducerTimerArbitration(ReceiveKind kind)
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receiveWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool sent = sendValues(channel, kind);
    const bool oneProducerWake = scheduler.hasSingleReadyTask();
    const bool noDuplicateWake = scheduler.resumeWithTimerInDequeueGap();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        receivedExpectedValues(state, kind);

    scheduler.releaseRetainedState();
    return started && sent && oneProducerWake && noDuplicateWake && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool runTimerProducerArbitration(ReceiveKind kind)
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receiveWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool timerFired = scheduler.fireTimer();
    const bool oneTimerWake = scheduler.hasSingleReadyTask();
    const bool sent = sendValues(channel, kind);
    const bool stillOneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    const bool timedOut = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.valueCount.load(std::memory_order_acquire) == 0 &&
        state.receivedTimeout.load(std::memory_order_acquire) &&
        callerOwnedBufferUnchanged(state, kind);
    const bool retainedValues = drainExpectedValues(channel, kind);

    scheduler.releaseRetainedState();
    return started && timerFired && oneTimerWake && sent && stillOneWake &&
        resumed && timedOut && retainedValues &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runStaleImmediateNotification(ReceiveKind kind)
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    int oldValue = 11;
    if (!channel.send(std::move(oldValue), true)) {
        return false;
    }
    auto consumedOldValue = channel.tryRecv();
    if (!consumedOldValue.has_value() || *consumedOldValue != 11 ||
        !channel.empty()) {
        return false;
    }

    QueuedRaceScheduler scheduler;
    ReceiveState state;
    auto task = receiveWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    if (!scheduler.scheduleImmediately(std::move(scheduled))) {
        return false;
    }

    // 重放第一条消息的延迟 immediately 通知；新 waiter 注册时通道已空。
    channel.notifyConsumer(0, 1, true);
    const bool staleNotificationIgnored = !scheduler.hasSingleReadyTask();

    bool sentCurrentValues = false;
    bool oneCurrentWake = false;
    bool resumed = false;
    if (staleNotificationIgnored) {
        sentCurrentValues = sendValues(channel, kind);
        oneCurrentWake = scheduler.hasSingleReadyTask();
        resumed = scheduler.runOne();
    } else {
        resumed = scheduler.runOne();
    }

    const bool receivedCurrentValues =
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        receivedExpectedValues(state, kind) && channel.empty();
    scheduler.releaseRetainedState();
    return staleNotificationIgnored && sentCurrentValues && oneCurrentWake &&
        resumed && receivedCurrentValues &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runCompletedWaiterSlotIsNotReused()
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    QueuedRaceScheduler scheduler;
    ReceiveState firstState;
    ReceiveState secondState;

    auto firstTask = receiveWithTimeout(&channel, ReceiveKind::kRecv, &firstState);
    TaskRef firstKeeper = detail::TaskAccess::taskRef(firstTask);
    TaskState* firstTaskState = firstKeeper.state();
    if (firstTaskState == nullptr) {
        return false;
    }
    TaskRef firstScheduled = detail::TaskAccess::detachTask(std::move(firstTask));
    if (!scheduler.scheduleImmediately(std::move(firstScheduled))) {
        return false;
    }

    int firstValue = 73;
    if (!channel.send(std::move(firstValue), true) ||
        !scheduler.hasSingleReadyTask()) {
        scheduler.releaseRetainedState();
        return false;
    }

    auto secondTask =
        receiveWithTimeout(&channel, ReceiveKind::kRecvBatched, &secondState);
    TaskRef secondKeeper = detail::TaskAccess::taskRef(secondTask);
    TaskState* secondTaskState = secondKeeper.state();
    if (secondTaskState == nullptr) {
        scheduler.releaseRetainedState();
        return false;
    }
    TaskRef secondScheduled = detail::TaskAccess::detachTask(std::move(secondTask));
    const bool secondStarted =
        scheduler.scheduleImmediately(std::move(secondScheduled));
    const bool secondRejected = secondState.completed.load(std::memory_order_acquire) &&
        secondState.resumeCount.load(std::memory_order_acquire) == 1 &&
        secondState.receivedNotReady.load(std::memory_order_acquire) &&
        !secondState.receivedTimeout.load(std::memory_order_acquire) &&
        secondState.valueCount.load(std::memory_order_acquire) == 0;
    const bool firstStillQueued = scheduler.hasSingleReadyTask();
    const bool firstResumed = scheduler.runOne();
    const bool firstCompleted = firstState.completed.load(std::memory_order_acquire) &&
        firstState.resumeCount.load(std::memory_order_acquire) == 1 &&
        receivedExpectedValues(firstState, ReceiveKind::kRecv);

    scheduler.releaseRetainedState();
    return secondStarted && secondRejected && firstStillQueued && firstResumed &&
        firstCompleted && channel.empty() &&
        firstTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        secondTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runImmediateWakeDuringRegistering()
{
    using unbounded_registering_test::g_gate;
    g_gate.entered.store(false, std::memory_order_relaxed);
    g_gate.proceed.store(false, std::memory_order_relaxed);
    g_gate.enabled.store(true, std::memory_order_release);

    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    QueuedRaceScheduler scheduler;
    ReceiveState state;
    auto task = receiveWithTimeout(&channel, ReceiveKind::kRecvBatched, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        g_gate.enabled.store(false, std::memory_order_release);
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    std::atomic<bool> started{false};
    std::thread starter([&]() {
        started.store(
            scheduler.scheduleImmediately(std::move(scheduled)),
            std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!g_gate.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool entered = g_gate.entered.load(std::memory_order_acquire);
    int value = 73;
    const bool sent = entered && channel.send(std::move(value), true);
    g_gate.proceed.store(true, std::memory_order_release);
    starter.join();
    g_gate.enabled.store(false, std::memory_order_release);

    const bool completed = started.load(std::memory_order_acquire) &&
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.valueCount.load(std::memory_order_acquire) == 1 &&
        state.valueSum.load(std::memory_order_acquire) == 73 &&
        !state.receivedTimeout.load(std::memory_order_acquire) &&
        !state.receivedNotReady.load(std::memory_order_acquire) && channel.empty();
    scheduler.releaseRetainedState();
    return entered && sent && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runImmediateWakeAfterWaiterClaim()
{
    using unbounded_registering_test::g_claimedGate;
    g_claimedGate.entered.store(false, std::memory_order_relaxed);
    g_claimedGate.proceed.store(false, std::memory_order_relaxed);
    g_claimedGate.enabled.store(true, std::memory_order_release);

    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    QueuedRaceScheduler scheduler;
    ReceiveState state;
    auto task = receiveWithTimeout(&channel, ReceiveKind::kRecvBatched, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        g_claimedGate.enabled.store(false, std::memory_order_release);
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    std::atomic<bool> started{false};
    std::thread starter([&]() {
        started.store(
            scheduler.scheduleImmediately(std::move(scheduled)),
            std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!g_claimedGate.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool entered = g_claimedGate.entered.load(std::memory_order_acquire);
    int firstValue = 73;
    const bool firstSent = entered && channel.send(std::move(firstValue), true);
    g_claimedGate.proceed.store(true, std::memory_order_release);
    starter.join();
    g_claimedGate.enabled.store(false, std::memory_order_release);

    // immediately=true 明确忽略 recvBatched 阈值；注册方应在当前栈同步重试。
    const bool completed = started.load(std::memory_order_acquire) &&
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.valueCount.load(std::memory_order_acquire) == 1 &&
        state.valueSum.load(std::memory_order_acquire) == 73 &&
        !state.receivedTimeout.load(std::memory_order_acquire) &&
        !state.receivedNotReady.load(std::memory_order_acquire) &&
        !scheduler.hasSingleReadyTask() && channel.empty();
    scheduler.releaseRetainedState();
    return entered && firstSent && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runStaleImmediateNotificationDuringRegistering()
{
    using unbounded_registering_test::g_gate;
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    int oldValue = 11;
    if (!channel.send(std::move(oldValue), true)) {
        return false;
    }
    auto consumedOldValue = channel.tryRecv();
    if (!consumedOldValue.has_value() || *consumedOldValue != 11 ||
        !channel.empty()) {
        return false;
    }

    g_gate.entered.store(false, std::memory_order_relaxed);
    g_gate.proceed.store(false, std::memory_order_relaxed);
    g_gate.enabled.store(true, std::memory_order_release);

    QueuedRaceScheduler scheduler;
    ReceiveState state;
    auto task = receiveWithTimeout(&channel, ReceiveKind::kRecvBatched, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        g_gate.enabled.store(false, std::memory_order_release);
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    std::atomic<bool> started{false};
    std::thread starter([&]() {
        started.store(
            scheduler.scheduleImmediately(std::move(scheduled)),
            std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!g_gate.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool entered = g_gate.entered.load(std::memory_order_acquire);
    if (entered) {
        // 重放已消费旧消息的延迟通知；当前可用数量为 0。
        channel.notifyConsumer(0, 1, true);
    }
    g_gate.proceed.store(true, std::memory_order_release);
    starter.join();
    g_gate.enabled.store(false, std::memory_order_release);

    const bool staleIgnored = started.load(std::memory_order_acquire) &&
        !state.completed.load(std::memory_order_acquire) &&
        !scheduler.hasSingleReadyTask();
    bool sent = false;
    bool resumed = false;
    if (staleIgnored) {
        sent = sendValues(channel, ReceiveKind::kRecvBatched);
        resumed = scheduler.hasSingleReadyTask() && scheduler.runOne();
    }
    const bool completed = resumed &&
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        receivedExpectedValues(state, ReceiveKind::kRecvBatched) &&
        !state.receivedNotReady.load(std::memory_order_acquire) && channel.empty();
    scheduler.releaseRetainedState();
    return entered && staleIgnored && sent && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runTimeoutResumeWaitsForProducerCleanup()
{
    using unbounded_registering_test::g_reclaimWaitObserved;
    using unbounded_registering_test::g_wakingGate;
    g_wakingGate.entered.store(false, std::memory_order_relaxed);
    g_wakingGate.proceed.store(false, std::memory_order_relaxed);
    g_wakingGate.enabled.store(true, std::memory_order_release);
    g_reclaimWaitObserved.store(false, std::memory_order_relaxed);

    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    QueuedRaceScheduler scheduler;
    ReceiveState state;
    auto task = receiveWithTimeout(&channel, ReceiveKind::kRecv, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        g_wakingGate.enabled.store(false, std::memory_order_release);
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    if (!scheduler.scheduleImmediately(std::move(scheduled))) {
        g_wakingGate.enabled.store(false, std::memory_order_release);
        return false;
    }

    std::atomic<bool> sent{false};
    std::thread producer([&]() {
        int value = 73;
        sent.store(channel.send(std::move(value), true),
                   std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!g_wakingGate.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool producerPaused =
        g_wakingGate.entered.load(std::memory_order_acquire);
    const bool timerFired = producerPaused && scheduler.fireTimer();
    const bool oneTimeoutWake = timerFired && scheduler.hasSingleReadyTask();

    std::atomic<bool> resumed{false};
    std::thread resumer;
    if (oneTimeoutWake) {
        resumer = std::thread([&]() {
            resumed.store(scheduler.runOne(), std::memory_order_release);
        });
    }
    while (!g_reclaimWaitObserved.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool reclaimWaitObserved =
        g_reclaimWaitObserved.load(std::memory_order_acquire);
    const bool resumeBlockedUntilCleanup =
        reclaimWaitObserved && !state.completed.load(std::memory_order_acquire);

    g_wakingGate.proceed.store(true, std::memory_order_release);
    producer.join();
    if (resumer.joinable()) {
        resumer.join();
    }
    g_wakingGate.enabled.store(false, std::memory_order_release);

    const bool timedOut = resumed.load(std::memory_order_acquire) &&
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedTimeout.load(std::memory_order_acquire) &&
        state.valueCount.load(std::memory_order_acquire) == 0;
    const bool retainedValue = drainExpectedValues(channel, ReceiveKind::kRecv);
    scheduler.releaseRetainedState();
    return producerPaused && timerFired && oneTimeoutWake &&
        resumeBlockedUntilCleanup && sent.load(std::memory_order_acquire) &&
        timedOut && retainedValue &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

}  // namespace

int main()
{
    if (!runStaleImmediateNotificationDuringRegistering()) {
        std::cerr << "[T157] stale immediate notification poisoned registering waiter\n";
        return 1;
    }
    if (!runImmediateWakeAfterWaiterClaim()) {
        std::cerr << "[T157] immediate wake was lost after waiter slot claim\n";
        return 1;
    }
    if (!runTimeoutResumeWaitsForProducerCleanup()) {
        std::cerr << "[T157] timeout resume overtook producer waiter cleanup\n";
        return 1;
    }
    if (!runImmediateWakeDuringRegistering()) {
        std::cerr << "[T157] immediate wake was lost during waiter registration\n";
        return 1;
    }
    if (!runCompletedWaiterSlotIsNotReused()) {
        std::cerr << "[T157] completed waiter slot was reused before await_resume\n";
        return 1;
    }
    for (ReceiveKind kind : kReceiveKinds) {
        if (!runStaleImmediateNotification(kind)) {
            std::cerr << "[T157] " << receiveKindName(kind)
                      << " stale immediate notification woke a new waiter\n";
            return 1;
        }
        if (!runImmediateCompletion(kind, true)) {
            std::cerr << "[T157] " << receiveKindName(kind)
                      << " operation-win addTimer=false failed\n";
            return 1;
        }
        if (!runImmediateCompletion(kind, false)) {
            std::cerr << "[T157] " << receiveKindName(kind)
                      << " timeout-win addTimer=false failed\n";
            return 1;
        }
        if (!runProducerTimerArbitration(kind)) {
            std::cerr << "[T157] " << receiveKindName(kind)
                      << " producer/timer arbitration admitted a duplicate wake\n";
            return 1;
        }
        if (!runTimerProducerArbitration(kind)) {
            std::cerr << "[T157] " << receiveKindName(kind)
                      << " timer/producer arbitration consumed a post-timeout value\n";
            return 1;
        }
    }

    std::cout << "T157-SpscTimeoutRace PASS\n";
    return 0;
}
