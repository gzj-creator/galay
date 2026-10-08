/**
 * @file t157_spsc_timeout_race.cc
 * @brief 验证 SPSC unbounded 三种接收等待体只由 producer 或 timeout 完成一次。
 */

#include "test/cpp/common/scheduler_test_adapter.h"
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
void pause_claimed() noexcept;
void pause() noexcept;
void pause_waking() noexcept;
void observe_reclaim_wait() noexcept;
}

#define GALAY_SPSC_UNBOUNDED_CLAIMED_TEST_POINT() \
    ::unbounded_registering_test::pause_claimed()
#define GALAY_SPSC_UNBOUNDED_REGISTERING_TEST_POINT() \
    ::unbounded_registering_test::pause()
#define GALAY_SPSC_UNBOUNDED_WAKING_TEST_POINT() \
    ::unbounded_registering_test::pause_waking()
#define GALAY_SPSC_UNBOUNDED_RECLAIM_WAIT_TEST_POINT() \
    ::unbounded_registering_test::observe_reclaim_wait()
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

void pause_claimed() noexcept
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

void pause_waking() noexcept
{
    if (!g_wakingGate.enabled.load(std::memory_order_acquire)) {
        return;
    }
    g_wakingGate.entered.store(true, std::memory_order_release);
    while (!g_wakingGate.proceed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void observe_reclaim_wait() noexcept
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

const char* receive_kind_name(ReceiveKind kind) noexcept
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

size_t expected_count(ReceiveKind kind) noexcept
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

bool send_values(galay::spsc::UnboundedChannel<int>& channel, ReceiveKind kind)
{
    if (kind == ReceiveKind::kRecv) {
        return channel.send(73);
    }
    std::vector<int> values{73, 74};
    return channel.send_batch(std::move(values));
}

class ImmediateRaceScheduler final : public detail::SchedulerTestAdapter<ImmediateRaceScheduler>
{
public:
    ImmediateRaceScheduler(galay::spsc::UnboundedChannel<int>* channel,
                           ReceiveKind kind,
                           bool publishValues) noexcept
        : m_channel(channel), m_kind(kind), m_publishValues(publishValues)
    {
    }

    std::expected<void, IOError> start() { return {}; }
    void stop() {}

    bool schedule(TaskRef task) noexcept
    {
        if (!bind_task(task)) {
            return false;
        }
        [[maybe_unused]] const int previousScheduleCalls =
            m_scheduleCalls.fetch_add(1, std::memory_order_relaxed);
        resume(task);
        return true;
    }

    // 兼容尚未提供专用 resume admission 的 Scheduler；新接口存在时仍会隐式覆盖。
    bool schedule_resume(TaskRef task) noexcept
    {
        return schedule(std::move(task));
    }

    bool schedule_deferred(TaskRef task) noexcept
    {
        return schedule(std::move(task));
    }

    bool schedule_immediately(TaskRef task) noexcept
    {
        if (!bind_task(task)) {
            return false;
        }
        resume(task);
        return true;
    }

    bool add_timer(Timer::ptr timer)
    {
        [[maybe_unused]] const int previousAddTimerCalls =
            m_addTimerCalls.fetch_add(1, std::memory_order_relaxed);
        if (m_publishValues) {
            m_sendSucceeded.store(
                send_values(*m_channel, m_kind), std::memory_order_release);
        }
        // 强制 WithTimeout 走 timeout_now()，覆盖 inner waiter 发布后的同步完成窗口。
        return false;
    }

    SchedulerType type() { return kParallelScheduler; }

    int schedule_calls() const noexcept
    {
        return m_scheduleCalls.load(std::memory_order_acquire);
    }

    int add_timer_calls() const noexcept
    {
        return m_addTimerCalls.load(std::memory_order_acquire);
    }

    bool send_succeeded() const noexcept
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

class QueuedRaceScheduler final : public detail::SchedulerTestAdapter<QueuedRaceScheduler>
{
public:
    std::expected<void, IOError> start() { return {}; }
    void stop() {}

    bool schedule(TaskRef task) noexcept
    {
        if (!bind_task(task)) {
            return false;
        }
        m_ready.push_back(std::move(task));
        return true;
    }

    // 兼容尚未提供专用 resume admission 的 Scheduler；新接口存在时仍会隐式覆盖。
    bool schedule_resume(TaskRef task) noexcept
    {
        return schedule(std::move(task));
    }

    bool schedule_deferred(TaskRef task) noexcept
    {
        return schedule(std::move(task));
    }

    bool schedule_immediately(TaskRef task) noexcept
    {
        if (!bind_task(task)) {
            return false;
        }
        resume(task);
        return true;
    }

    bool add_timer(Timer::ptr timer)
    {
        m_timer = std::move(timer);
        return true;
    }

    SchedulerType type() { return kParallelScheduler; }

    bool has_single_ready_task() const noexcept { return m_ready.size() == 1; }

    bool fire_timer()
    {
        if (!m_timer) {
            return false;
        }
        m_timer->handle_timeout();
        return true;
    }

    bool run_one()
    {
        if (m_ready.empty()) {
            return false;
        }
        TaskRef task = std::move(m_ready.front());
        m_ready.pop_front();
        resume(task);
        return true;
    }

    bool resume_with_timer_in_dequeue_gap()
    {
        if (m_ready.empty() || !m_timer) {
            return false;
        }
        TaskRef task = std::move(m_ready.front());
        m_ready.pop_front();
        TaskState* state = task.state();
        if (state == nullptr || !state->m_handle ||
            state->is_done()) {
            return false;
        }

        // 精确模拟 resume_task_state() 在 dequeue 后、handle.resume() 前的窗口。
        state->m_queued.store(false, std::memory_order_relaxed);
        state->m_resume_owner_only.store(false, std::memory_order_relaxed);
        m_timer->handle_timeout();
        const bool noDuplicateWake = m_ready.empty();
        state->m_handle.resume();
        return noDuplicateWake;
    }

    void release_retained_state()
    {
        m_ready.clear();
        m_timer.reset();
    }

private:
    std::deque<TaskRef> m_ready;
    Timer::ptr m_timer;
};

Task<void> receive_with_timeout(galay::spsc::UnboundedChannel<int>* channel,
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
        auto result = co_await channel->recv_batch(2).timeout(1h);
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
        auto result = co_await channel->recv_batch_to(
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
        auto result = co_await channel->recv_batched(2).timeout(1h);
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

bool received_expected_values(const ReceiveState& state, ReceiveKind kind) noexcept
{
    const size_t count = expected_count(kind);
    const int sum = kind == ReceiveKind::kRecv ? 73 : 147;
    return state.valueCount.load(std::memory_order_acquire) == count &&
        state.valueSum.load(std::memory_order_acquire) == sum &&
        (kind != ReceiveKind::kRecvBatchTo ||
         state.batchOutput == std::array<int, 2>{73, 74}) &&
        !state.receivedTimeout.load(std::memory_order_acquire);
}

bool caller_owned_buffer_unchanged(const ReceiveState& state,
                                ReceiveKind kind) noexcept
{
    return kind != ReceiveKind::kRecvBatchTo ||
        state.batchOutput == std::array<int, 2>{-1, -1};
}

bool drain_expected_values(galay::spsc::UnboundedChannel<int>& channel,
                         ReceiveKind kind)
{
    std::array<int, 2> values{-1, -1};
    const size_t count = channel.try_recv_batch(
        std::span<int>(values).first(expected_count(kind)));
    if (count != expected_count(kind)) {
        return false;
    }
    int sum = 0;
    for (size_t index = 0; index < count; ++index) {
        sum += values[index];
    }
    const int expectedSum = kind == ReceiveKind::kRecv ? 73 : 147;
    return sum == expectedSum && channel.empty();
}

bool run_immediate_completion(ReceiveKind kind, bool publishValues)
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    ImmediateRaceScheduler scheduler(&channel, kind, publishValues);
    ReceiveState state;

    auto task = receive_with_timeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool correctResult = publishValues
        ? received_expected_values(state, kind) && scheduler.send_succeeded()
        : state.valueCount.load(std::memory_order_acquire) == 0 &&
            state.receivedTimeout.load(std::memory_order_acquire) &&
            caller_owned_buffer_unchanged(state, kind);
    const int expectedScheduleCalls = publishValues ? 1 : 0;

    return started && state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 && correctResult &&
        scheduler.add_timer_calls() == 1 &&
        scheduler.schedule_calls() == expectedScheduleCalls &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool run_producer_timer_arbitration(ReceiveKind kind)
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receive_with_timeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool sent = send_values(channel, kind);
    const bool oneProducerWake = scheduler.has_single_ready_task();
    const bool noDuplicateWake = scheduler.resume_with_timer_in_dequeue_gap();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        received_expected_values(state, kind);

    scheduler.release_retained_state();
    return started && sent && oneProducerWake && noDuplicateWake && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool run_timer_producer_arbitration(ReceiveKind kind)
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receive_with_timeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool timerFired = scheduler.fire_timer();
    const bool oneTimerWake = scheduler.has_single_ready_task();
    const bool sent = send_values(channel, kind);
    const bool stillOneWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    const bool timedOut = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.valueCount.load(std::memory_order_acquire) == 0 &&
        state.receivedTimeout.load(std::memory_order_acquire) &&
        caller_owned_buffer_unchanged(state, kind);
    const bool retainedValues = drain_expected_values(channel, kind);

    scheduler.release_retained_state();
    return started && timerFired && oneTimerWake && sent && stillOneWake &&
        resumed && timedOut && retainedValues &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_stale_immediate_notification(ReceiveKind kind)
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    int oldValue = 11;
    if (!channel.send(std::move(oldValue), true)) {
        return false;
    }
    auto consumedOldValue = channel.try_recv();
    if (!consumedOldValue.has_value() || *consumedOldValue != 11 ||
        !channel.empty()) {
        return false;
    }

    QueuedRaceScheduler scheduler;
    ReceiveState state;
    auto task = receive_with_timeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    if (!scheduler.schedule_immediately(std::move(scheduled))) {
        return false;
    }

    // 重放第一条消息的延迟 immediately 通知；新 waiter 注册时通道已空。
    channel.notify_consumer(0, 1, true);
    const bool staleNotificationIgnored = !scheduler.has_single_ready_task();

    bool sentCurrentValues = false;
    bool oneCurrentWake = false;
    bool resumed = false;
    if (staleNotificationIgnored) {
        sentCurrentValues = send_values(channel, kind);
        oneCurrentWake = scheduler.has_single_ready_task();
        resumed = scheduler.run_one();
    } else {
        resumed = scheduler.run_one();
    }

    const bool receivedCurrentValues =
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        received_expected_values(state, kind) && channel.empty();
    scheduler.release_retained_state();
    return staleNotificationIgnored && sentCurrentValues && oneCurrentWake &&
        resumed && receivedCurrentValues &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_completed_waiter_slot_is_not_reused()
{
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    QueuedRaceScheduler scheduler;
    ReceiveState firstState;
    ReceiveState secondState;

    auto firstTask = receive_with_timeout(&channel, ReceiveKind::kRecv, &firstState);
    TaskRef firstKeeper = detail::TaskAccess::task_ref(firstTask);
    TaskState* firstTaskState = firstKeeper.state();
    if (firstTaskState == nullptr) {
        return false;
    }
    TaskRef firstScheduled = detail::TaskAccess::detach_task(std::move(firstTask));
    if (!scheduler.schedule_immediately(std::move(firstScheduled))) {
        return false;
    }

    int firstValue = 73;
    if (!channel.send(std::move(firstValue), true) ||
        !scheduler.has_single_ready_task()) {
        scheduler.release_retained_state();
        return false;
    }

    auto secondTask =
        receive_with_timeout(&channel, ReceiveKind::kRecvBatched, &secondState);
    TaskRef secondKeeper = detail::TaskAccess::task_ref(secondTask);
    TaskState* secondTaskState = secondKeeper.state();
    if (secondTaskState == nullptr) {
        scheduler.release_retained_state();
        return false;
    }
    TaskRef secondScheduled = detail::TaskAccess::detach_task(std::move(secondTask));
    const bool secondStarted =
        scheduler.schedule_immediately(std::move(secondScheduled));
    const bool secondRejected = secondState.completed.load(std::memory_order_acquire) &&
        secondState.resumeCount.load(std::memory_order_acquire) == 1 &&
        secondState.receivedNotReady.load(std::memory_order_acquire) &&
        !secondState.receivedTimeout.load(std::memory_order_acquire) &&
        secondState.valueCount.load(std::memory_order_acquire) == 0;
    const bool firstStillQueued = scheduler.has_single_ready_task();
    const bool firstResumed = scheduler.run_one();
    const bool firstCompleted = firstState.completed.load(std::memory_order_acquire) &&
        firstState.resumeCount.load(std::memory_order_acquire) == 1 &&
        received_expected_values(firstState, ReceiveKind::kRecv);

    scheduler.release_retained_state();
    return secondStarted && secondRejected && firstStillQueued && firstResumed &&
        firstCompleted && channel.empty() &&
        firstTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        secondTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_immediate_wake_during_registering()
{
    using unbounded_registering_test::g_gate;
    g_gate.entered.store(false, std::memory_order_relaxed);
    g_gate.proceed.store(false, std::memory_order_relaxed);
    g_gate.enabled.store(true, std::memory_order_release);

    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    QueuedRaceScheduler scheduler;
    ReceiveState state;
    auto task = receive_with_timeout(&channel, ReceiveKind::kRecvBatched, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        g_gate.enabled.store(false, std::memory_order_release);
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    std::atomic<bool> started{false};
    std::thread starter([&]() {
        started.store(
            scheduler.schedule_immediately(std::move(scheduled)),
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
    scheduler.release_retained_state();
    return entered && sent && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_immediate_wake_after_waiter_claim()
{
    using unbounded_registering_test::g_claimedGate;
    g_claimedGate.entered.store(false, std::memory_order_relaxed);
    g_claimedGate.proceed.store(false, std::memory_order_relaxed);
    g_claimedGate.enabled.store(true, std::memory_order_release);

    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    QueuedRaceScheduler scheduler;
    ReceiveState state;
    auto task = receive_with_timeout(&channel, ReceiveKind::kRecvBatched, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        g_claimedGate.enabled.store(false, std::memory_order_release);
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    std::atomic<bool> started{false};
    std::thread starter([&]() {
        started.store(
            scheduler.schedule_immediately(std::move(scheduled)),
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

    // immediately=true 明确忽略 recv_batched 阈值；注册方应在当前栈同步重试。
    const bool completed = started.load(std::memory_order_acquire) &&
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.valueCount.load(std::memory_order_acquire) == 1 &&
        state.valueSum.load(std::memory_order_acquire) == 73 &&
        !state.receivedTimeout.load(std::memory_order_acquire) &&
        !state.receivedNotReady.load(std::memory_order_acquire) &&
        !scheduler.has_single_ready_task() && channel.empty();
    scheduler.release_retained_state();
    return entered && firstSent && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_stale_immediate_notification_during_registering()
{
    using unbounded_registering_test::g_gate;
    galay::spsc::UnboundedChannel<int> channel(galay::spsc::WakeMode::Deferred);
    int oldValue = 11;
    if (!channel.send(std::move(oldValue), true)) {
        return false;
    }
    auto consumedOldValue = channel.try_recv();
    if (!consumedOldValue.has_value() || *consumedOldValue != 11 ||
        !channel.empty()) {
        return false;
    }

    g_gate.entered.store(false, std::memory_order_relaxed);
    g_gate.proceed.store(false, std::memory_order_relaxed);
    g_gate.enabled.store(true, std::memory_order_release);

    QueuedRaceScheduler scheduler;
    ReceiveState state;
    auto task = receive_with_timeout(&channel, ReceiveKind::kRecvBatched, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        g_gate.enabled.store(false, std::memory_order_release);
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    std::atomic<bool> started{false};
    std::thread starter([&]() {
        started.store(
            scheduler.schedule_immediately(std::move(scheduled)),
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
        channel.notify_consumer(0, 1, true);
    }
    g_gate.proceed.store(true, std::memory_order_release);
    starter.join();
    g_gate.enabled.store(false, std::memory_order_release);

    const bool staleIgnored = started.load(std::memory_order_acquire) &&
        !state.completed.load(std::memory_order_acquire) &&
        !scheduler.has_single_ready_task();
    bool sent = false;
    bool resumed = false;
    if (staleIgnored) {
        sent = send_values(channel, ReceiveKind::kRecvBatched);
        resumed = scheduler.has_single_ready_task() && scheduler.run_one();
    }
    const bool completed = resumed &&
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        received_expected_values(state, ReceiveKind::kRecvBatched) &&
        !state.receivedNotReady.load(std::memory_order_acquire) && channel.empty();
    scheduler.release_retained_state();
    return entered && staleIgnored && sent && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_timeout_resume_waits_for_producer_cleanup()
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
    auto task = receive_with_timeout(&channel, ReceiveKind::kRecv, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        g_wakingGate.enabled.store(false, std::memory_order_release);
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    if (!scheduler.schedule_immediately(std::move(scheduled))) {
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
    const bool timerFired = producerPaused && scheduler.fire_timer();
    const bool oneTimeoutWake = timerFired && scheduler.has_single_ready_task();

    std::atomic<bool> resumed{false};
    std::thread resumer;
    if (oneTimeoutWake) {
        resumer = std::thread([&]() {
            resumed.store(scheduler.run_one(), std::memory_order_release);
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
    const bool retainedValue = drain_expected_values(channel, ReceiveKind::kRecv);
    scheduler.release_retained_state();
    return producerPaused && timerFired && oneTimeoutWake &&
        resumeBlockedUntilCleanup && sent.load(std::memory_order_acquire) &&
        timedOut && retainedValue &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

}  // namespace

int main()
{
    if (!run_stale_immediate_notification_during_registering()) {
        std::cerr << "[T157] stale immediate notification poisoned registering waiter\n";
        return 1;
    }
    if (!run_immediate_wake_after_waiter_claim()) {
        std::cerr << "[T157] immediate wake was lost after waiter slot claim\n";
        return 1;
    }
    if (!run_timeout_resume_waits_for_producer_cleanup()) {
        std::cerr << "[T157] timeout resume overtook producer waiter cleanup\n";
        return 1;
    }
    if (!run_immediate_wake_during_registering()) {
        std::cerr << "[T157] immediate wake was lost during waiter registration\n";
        return 1;
    }
    if (!run_completed_waiter_slot_is_not_reused()) {
        std::cerr << "[T157] completed waiter slot was reused before await_resume\n";
        return 1;
    }
    for (ReceiveKind kind : kReceiveKinds) {
        if (!run_stale_immediate_notification(kind)) {
            std::cerr << "[T157] " << receive_kind_name(kind)
                      << " stale immediate notification woke a new waiter\n";
            return 1;
        }
        if (!run_immediate_completion(kind, true)) {
            std::cerr << "[T157] " << receive_kind_name(kind)
                      << " operation-win addTimer=false failed\n";
            return 1;
        }
        if (!run_immediate_completion(kind, false)) {
            std::cerr << "[T157] " << receive_kind_name(kind)
                      << " timeout-win addTimer=false failed\n";
            return 1;
        }
        if (!run_producer_timer_arbitration(kind)) {
            std::cerr << "[T157] " << receive_kind_name(kind)
                      << " producer/timer arbitration admitted a duplicate wake\n";
            return 1;
        }
        if (!run_timer_producer_arbitration(kind)) {
            std::cerr << "[T157] " << receive_kind_name(kind)
                      << " timer/producer arbitration consumed a post-timeout value\n";
            return 1;
        }
    }

    std::cout << "T157-SpscTimeoutRace PASS\n";
    return 0;
}
