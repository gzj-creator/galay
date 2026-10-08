/**
 * @file t162_mpmc_bounded_timeout_race.cc
 * @brief 验证 MPMC bounded send/recv timeout 与对端操作只产生一个完成者。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/bounded_channel.h>
#include "test/cpp/common/scheduler_test_adapter.h"
#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

namespace galay::mpmc {

struct BoundedChannelTestAccess
{
    template <BoundedValue T>
    static bool enqueue_recv_waiter(
        BoundedChannel<T>& channel,
        const std::shared_ptr<BoundedChannelWaiter<T>>& waiter)
    {
        return channel.enqueue_waiter(channel.m_recvWaiters, waiter);
    }

    template <BoundedValue T>
    static bool enqueue_value(BoundedChannel<T>& channel, T&& value)
    {
        return channel.ring_enqueue(std::move(value));
    }

    template <BoundedValue T>
    static void request_recv_pump(BoundedChannel<T>& channel)
    {
        channel.request_pump(channel.kRecvWork);
    }

    template <BoundedValue T>
    static bool pump_idle(const BoundedChannel<T>& channel)
    {
        return channel.m_pumpState.load(std::memory_order_acquire) == 0;
    }

    template <BoundedValue T>
    static void seed_empty_position(BoundedChannel<T>& channel,
                                  uint64_t position)
    {
        channel.m_head.store(position, std::memory_order_relaxed);
        channel.m_tail.store(position, std::memory_order_relaxed);
        const uint64_t base = position & ~static_cast<uint64_t>(channel.m_mask);
        for (size_t index = 0; index < channel.m_capacity; ++index) {
            uint64_t sequence = base + index;
            if (sequence < position) {
                sequence += channel.m_capacity;
            }
            channel.m_slots[index].sequence.store(sequence,
                                                  std::memory_order_relaxed);
        }
    }

    template <BoundedValue T>
    static uint64_t tail_position(const BoundedChannel<T>& channel)
    {
        return static_cast<uint64_t>(
            channel.m_tail.load(std::memory_order_acquire)) &
            ((uint64_t{1} << 63U) - 1U);
    }
};

} // namespace galay::mpmc

namespace {

using namespace galay::kernel;
using namespace std::chrono_literals;

struct OperationState
{
    std::atomic<int> resumeCount{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> succeeded{false};
    std::atomic<bool> closed{false};
    std::atomic<bool> timedOut{false};
};

struct BlockingValue
{
    int value{0};
    std::atomic<bool>* moveEntered{nullptr};
    std::atomic<bool>* releaseMove{nullptr};

    BlockingValue() noexcept = default;

    BlockingValue(int input,
                  std::atomic<bool>* entered,
                  std::atomic<bool>* release) noexcept
        : value(input), moveEntered(entered), releaseMove(release)
    {
    }

    BlockingValue(const BlockingValue&) = delete;
    BlockingValue& operator=(const BlockingValue&) = delete;

    BlockingValue(BlockingValue&& other) noexcept
        : value(other.value)
        , moveEntered(other.moveEntered)
        , releaseMove(other.releaseMove)
    {
        if (moveEntered != nullptr && releaseMove != nullptr) {
            moveEntered->store(true, std::memory_order_release);
            while (!releaseMove->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    }

    BlockingValue& operator=(BlockingValue&& other) noexcept
    {
        value = other.value;
        moveEntered = other.moveEntered;
        releaseMove = other.releaseMove;
        return *this;
    }
};

struct BlockingReceiveState
{
    std::atomic<bool> completed{false};
    std::atomic<bool> succeeded{false};
    std::atomic<bool> closed{false};
};

enum class ImmediateAction {
    kNone,
    kPublishValue,
    kReleaseSlot,
};

class ImmediateRaceScheduler final : public detail::SchedulerTestAdapter<ImmediateRaceScheduler>
{
public:
    ImmediateRaceScheduler(galay::mpmc::BoundedChannel<int>* channel,
                           ImmediateAction action) noexcept
        : m_channel(channel), m_action(action)
    {
    }

    std::expected<void, IOError> start() { return {}; }
    void stop() {}

    bool schedule(TaskRef task) noexcept
    {
        if (!bind_task(task)) {
            return false;
        }
        m_scheduleCalls.fetch_add(1, std::memory_order_relaxed);
        resume(task);
        return true;
    }

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
        m_addTimerCalls.fetch_add(1, std::memory_order_relaxed);
        if (m_action == ImmediateAction::kPublishValue) {
            m_actionSucceeded.store(m_channel->try_send(73), std::memory_order_release);
        } else if (m_action == ImmediateAction::kReleaseSlot) {
            auto value = m_channel->try_recv();
            m_actionSucceeded.store(
                value.has_value() && *value == 1,
                std::memory_order_release);
        }
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

    bool action_succeeded() const noexcept
    {
        return m_actionSucceeded.load(std::memory_order_acquire);
    }

private:
    galay::mpmc::BoundedChannel<int>* m_channel;
    const ImmediateAction m_action;
    std::atomic<int> m_scheduleCalls{0};
    std::atomic<int> m_addTimerCalls{0};
    std::atomic<bool> m_actionSucceeded{false};
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
        m_timers.push_back(std::move(timer));
        return true;
    }

    SchedulerType type() { return kParallelScheduler; }

    bool has_single_ready_task() const noexcept { return m_ready.size() == 1; }

    size_t ready_count() const noexcept { return m_ready.size(); }

    bool fire_timer()
    {
        return fire_timer(0);
    }

    bool fire_timer(size_t index)
    {
        if (index >= m_timers.size() || !m_timers[index]) {
            return false;
        }
        m_timers[index]->handle_timeout();
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
        if (m_ready.empty() || m_timers.empty() || !m_timers.front()) {
            return false;
        }
        TaskRef task = std::move(m_ready.front());
        m_ready.pop_front();
        TaskState* state = task.state();
        if (state == nullptr || !state->m_handle ||
            state->is_done()) {
            return false;
        }

        state->m_queued.store(false, std::memory_order_relaxed);
        state->m_resume_owner_only.store(false, std::memory_order_relaxed);
        m_timers.front()->handle_timeout();
        const bool noDuplicateWake = m_ready.empty();
        state->m_handle.resume();
        return noDuplicateWake;
    }

    void release_retained_state()
    {
        m_ready.clear();
        m_timers.clear();
    }

private:
    std::deque<TaskRef> m_ready;
    std::vector<Timer::ptr> m_timers;
};

template <bool Batch>
Task<void> receive_with_timeout(galay::mpmc::BoundedChannel<int>* channel,
                              OperationState* state)
{
    if constexpr (Batch) {
        auto result = co_await channel->recv_batch(4).timeout(1h);
        state->resumeCount.fetch_add(1, std::memory_order_relaxed);
        if (result.has_value()) {
            state->succeeded.store(
                result->size() == 1 && result->front() == 73,
                std::memory_order_release);
        } else {
            state->closed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
            state->timedOut.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
        }
    } else {
        auto result = co_await channel->recv().timeout(1h);
        state->resumeCount.fetch_add(1, std::memory_order_relaxed);
        if (result.has_value()) {
            state->succeeded.store(*result == 73, std::memory_order_release);
        } else {
            state->closed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
            state->timedOut.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
        }
    }
    state->completed.store(true, std::memory_order_release);
    co_return;
}

Task<void> send_with_timeout(galay::mpmc::BoundedChannel<int>* channel,
                           OperationState* state,
                           int value = 73)
{
    auto result = co_await channel->send(std::move(value)).timeout(1h);
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    if (result.has_value()) {
        state->succeeded.store(true, std::memory_order_release);
    } else {
        state->closed.store(
            IOError::contains(result.error().code(), kClosed),
            std::memory_order_release);
        state->timedOut.store(
            IOError::contains(result.error().code(), kTimeout),
            std::memory_order_release);
    }
    state->completed.store(true, std::memory_order_release);
    co_return;
}

Task<void> receive_blocking_value(
    galay::mpmc::BoundedChannel<BlockingValue>* channel,
    BlockingReceiveState* state)
{
    auto result = co_await channel->recv();
    if (result.has_value()) {
        state->succeeded.store(result->value == 73, std::memory_order_release);
    } else {
        state->closed.store(
            IOError::contains(result.error().code(), kClosed),
            std::memory_order_release);
    }
    state->completed.store(true, std::memory_order_release);
    co_return;
}

bool completed_once(const OperationState& state, bool expectSuccess)
{
    return state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.succeeded.load(std::memory_order_acquire) == expectSuccess &&
        !state.closed.load(std::memory_order_acquire) &&
        state.timedOut.load(std::memory_order_acquire) != expectSuccess;
}

bool completed_closed_once(const OperationState& state)
{
    return state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        !state.succeeded.load(std::memory_order_acquire) &&
        state.closed.load(std::memory_order_acquire) &&
        !state.timedOut.load(std::memory_order_acquire);
}

template <bool Batch>
bool run_recv_immediate(bool publishValue)
{
    galay::mpmc::BoundedChannel<int> channel(2);
    ImmediateRaceScheduler scheduler(
        &channel,
        publishValue ? ImmediateAction::kPublishValue : ImmediateAction::kNone);
    OperationState state;
    auto task = receive_with_timeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    return started && completed_once(state, publishValue) &&
        scheduler.add_timer_calls() == 1 && scheduler.schedule_calls() == 0 &&
        (!publishValue || scheduler.action_succeeded()) &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool run_recv_producer_first()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = receive_with_timeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool sent = channel.try_send(73);
    const bool oneWake = scheduler.has_single_ready_task();
    const bool noDuplicate = scheduler.resume_with_timer_in_dequeue_gap();
    const bool passed = completed_once(state, true);
    scheduler.release_retained_state();
    return started && sent && oneWake && noDuplicate && passed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool run_recv_timer_first()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = receive_with_timeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool timerFired = scheduler.fire_timer();
    const bool oneWake = scheduler.has_single_ready_task();
    const bool sent = channel.try_send(73);
    const bool stillOneWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    auto retained = channel.try_recv();
    const bool passed = completed_once(state, false);
    scheduler.release_retained_state();
    return started && timerFired && oneWake && sent && stillOneWake && resumed &&
        passed && retained.has_value() && *retained == 73 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool fill_channel(galay::mpmc::BoundedChannel<int>& channel)
{
    return channel.try_send(1) && channel.try_send(2) && channel.full();
}

bool run_send_immediate(bool releaseSlot)
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fill_channel(channel)) {
        return false;
    }
    ImmediateRaceScheduler scheduler(
        &channel,
        releaseSlot ? ImmediateAction::kReleaseSlot : ImmediateAction::kNone);
    OperationState state;
    auto task = send_with_timeout(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    auto first = channel.try_recv();
    auto second = channel.try_recv();
    const bool queueCorrect = releaseSlot
        ? first.has_value() && second.has_value() && *first == 2 && *second == 73
        : first.has_value() && second.has_value() && *first == 1 && *second == 2;
    return started && completed_once(state, releaseSlot) && queueCorrect &&
        scheduler.add_timer_calls() == 1 && scheduler.schedule_calls() == 0 &&
        (!releaseSlot || scheduler.action_succeeded()) &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool run_send_consumer_first()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fill_channel(channel)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = send_with_timeout(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    auto released = channel.try_recv();
    const bool oneWake = scheduler.has_single_ready_task();
    const bool noDuplicate = scheduler.resume_with_timer_in_dequeue_gap();
    auto first = channel.try_recv();
    auto second = channel.try_recv();
    const bool queueCorrect = released.has_value() && *released == 1 &&
        first.has_value() && *first == 2 && second.has_value() && *second == 73;
    const bool passed = completed_once(state, true);
    scheduler.release_retained_state();
    return started && oneWake && noDuplicate && queueCorrect && passed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool run_send_timer_first()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fill_channel(channel)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = send_with_timeout(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool timerFired = scheduler.fire_timer();
    const bool oneWake = scheduler.has_single_ready_task();
    auto released = channel.try_recv();
    const bool stillOneWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    auto remaining = channel.try_recv();
    auto unexpected = channel.try_recv();
    const bool queueCorrect = released.has_value() && *released == 1 &&
        remaining.has_value() && *remaining == 2 && !unexpected.has_value();
    const bool passed = completed_once(state, false);
    scheduler.release_retained_state();
    return started && timerFired && oneWake && stillOneWake && resumed &&
        queueCorrect && passed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool run_recv_close_first()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = receive_with_timeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    channel.close();
    const bool oneWake = scheduler.has_single_ready_task();
    const bool noDuplicate = scheduler.resume_with_timer_in_dequeue_gap();
    const bool passed = completed_closed_once(state);
    scheduler.release_retained_state();
    return started && oneWake && noDuplicate && passed && channel.empty() &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

template <bool Batch>
bool run_recv_timer_then_close()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = receive_with_timeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool timerFired = scheduler.fire_timer();
    const bool oneWake = scheduler.has_single_ready_task();
    channel.close();
    const bool stillOneWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    const bool passed = completed_once(state, false);
    scheduler.release_retained_state();
    return started && timerFired && oneWake && stillOneWake && resumed && passed &&
        channel.empty() && taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_send_close_first()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fill_channel(channel)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = send_with_timeout(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    channel.close();
    const bool oneWake = scheduler.has_single_ready_task();
    const bool noDuplicate = scheduler.resume_with_timer_in_dequeue_gap();
    auto first = channel.try_recv();
    auto second = channel.try_recv();
    const bool queueCorrect = first.has_value() && second.has_value() &&
        *first == 1 && *second == 2 && !channel.try_recv().has_value();
    const bool passed = completed_closed_once(state);
    scheduler.release_retained_state();
    return started && oneWake && noDuplicate && queueCorrect && passed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_send_timer_then_close()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fill_channel(channel)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = send_with_timeout(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool timerFired = scheduler.fire_timer();
    const bool oneWake = scheduler.has_single_ready_task();
    channel.close();
    const bool stillOneWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    auto first = channel.try_recv();
    auto second = channel.try_recv();
    const bool queueCorrect = first.has_value() && second.has_value() &&
        *first == 1 && *second == 2 && !channel.try_recv().has_value();
    const bool passed = completed_once(state, false);
    scheduler.release_retained_state();
    return started && timerFired && oneWake && stillOneWake && resumed &&
        queueCorrect && passed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

template <bool Batch>
bool run_timed_out_recv_does_not_swallow_message()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    OperationState timedOutState;
    OperationState liveState;
    auto timedOutTask = receive_with_timeout<Batch>(&channel, &timedOutState);
    auto liveTask = receive_with_timeout<Batch>(&channel, &liveState);
    TaskRef timedOutKeeper = detail::TaskAccess::task_ref(timedOutTask);
    TaskRef liveKeeper = detail::TaskAccess::task_ref(liveTask);
    TaskState* timedOutTaskState = timedOutKeeper.state();
    TaskState* liveTaskState = liveKeeper.state();
    if (timedOutTaskState == nullptr || liveTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled = detail::TaskAccess::detach_task(std::move(timedOutTask));
    TaskRef secondScheduled = detail::TaskAccess::detach_task(std::move(liveTask));
    const bool firstStarted = scheduler.schedule_immediately(std::move(firstScheduled));
    const bool secondStarted = scheduler.schedule_immediately(std::move(secondScheduled));
    const bool timerFired = scheduler.fire_timer(0);
    const bool timeoutReady = scheduler.ready_count() == 1;
    const bool sent = channel.try_send(73);
    const bool bothReady = scheduler.ready_count() == 2;
    const bool firstResumed = scheduler.run_one();
    const bool secondResumed = scheduler.run_one();
    const bool passed = completed_once(timedOutState, false) &&
        completed_once(liveState, true);
    scheduler.release_retained_state();
    return firstStarted && secondStarted && timerFired && timeoutReady && sent &&
        bothReady && firstResumed && secondResumed && passed && channel.empty() &&
        timedOutTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        liveTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_timed_out_send_does_not_swallow_slot()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fill_channel(channel)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState timedOutState;
    OperationState liveState;
    auto timedOutTask = send_with_timeout(&channel, &timedOutState, 73);
    auto liveTask = send_with_timeout(&channel, &liveState, 74);
    TaskRef timedOutKeeper = detail::TaskAccess::task_ref(timedOutTask);
    TaskRef liveKeeper = detail::TaskAccess::task_ref(liveTask);
    TaskState* timedOutTaskState = timedOutKeeper.state();
    TaskState* liveTaskState = liveKeeper.state();
    if (timedOutTaskState == nullptr || liveTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled = detail::TaskAccess::detach_task(std::move(timedOutTask));
    TaskRef secondScheduled = detail::TaskAccess::detach_task(std::move(liveTask));
    const bool firstStarted = scheduler.schedule_immediately(std::move(firstScheduled));
    const bool secondStarted = scheduler.schedule_immediately(std::move(secondScheduled));
    const bool timerFired = scheduler.fire_timer(0);
    const bool timeoutReady = scheduler.ready_count() == 1;
    auto released = channel.try_recv();
    const bool bothReady = scheduler.ready_count() == 2;
    const bool firstResumed = scheduler.run_one();
    const bool secondResumed = scheduler.run_one();
    auto first = channel.try_recv();
    auto second = channel.try_recv();
    const bool queueCorrect = released.has_value() && *released == 1 &&
        first.has_value() && *first == 2 && second.has_value() && *second == 74 &&
        !channel.try_recv().has_value();
    const bool passed = completed_once(timedOutState, false) &&
        completed_once(liveState, true);
    scheduler.release_retained_state();
    return firstStarted && secondStarted && timerFired && timeoutReady && bothReady &&
        firstResumed && secondResumed && queueCorrect && passed &&
        timedOutTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        liveTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_recv_pump_skips_cancelled_waiter()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    auto cancelled = std::make_shared<galay::mpmc::BoundedChannelWaiter<int>>(
        galay::kernel::Waker());
    auto live = std::make_shared<galay::mpmc::BoundedChannelWaiter<int>>(
        galay::kernel::Waker());
    if (!galay::mpmc::BoundedChannelTestAccess::enqueue_recv_waiter(channel, cancelled) ||
        !galay::mpmc::BoundedChannelTestAccess::enqueue_recv_waiter(channel, live)) {
        return false;
    }
    cancelled->state.store(galay::mpmc::BoundedWaiterState::kCancelled,
                           std::memory_order_release);
    if (!galay::mpmc::BoundedChannelTestAccess::enqueue_value(channel, 73)) {
        return false;
    }

    galay::mpmc::BoundedChannelTestAccess::request_recv_pump(channel);
    return live->state.load(std::memory_order_acquire) ==
            galay::mpmc::BoundedWaiterState::kFulfilled &&
        live->value.has_value() && *live->value == 73 && channel.empty() &&
        galay::mpmc::BoundedChannelTestAccess::pump_idle(channel);
}

bool run_close_waits_for_in_flight_send()
{
    galay::mpmc::BoundedChannel<BlockingValue> channel(2);
    QueuedRaceScheduler scheduler;
    BlockingReceiveState receiveState;
    std::atomic<bool> moveEntered{false};
    std::atomic<bool> releaseMove{false};
    std::atomic<bool> sendSucceeded{false};

    std::thread producer([&] {
        BlockingValue value(73, &moveEntered, &releaseMove);
        sendSucceeded.store(channel.try_send(std::move(value)),
                            std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!moveEntered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (!moveEntered.load(std::memory_order_acquire)) {
        releaseMove.store(true, std::memory_order_release);
        producer.join();
        return false;
    }

    channel.close();
    auto task = receive_blocking_value(&channel, &receiveState);
    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool stillWaiting = !receiveState.completed.load(std::memory_order_acquire);

    releaseMove.store(true, std::memory_order_release);
    producer.join();
    const bool oneWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    scheduler.release_retained_state();
    return started && stillWaiting && sendSucceeded.load(std::memory_order_acquire) &&
        oneWake && resumed && receiveState.completed.load(std::memory_order_acquire) &&
        receiveState.succeeded.load(std::memory_order_acquire) &&
        !receiveState.closed.load(std::memory_order_acquire) && channel.empty();
}

bool run_tail_close_bit_boundary()
{
    constexpr uint64_t kPositionMask = (uint64_t{1} << 63U) - 1U;

    galay::mpmc::BoundedChannel<int> lastReservation(2);
    galay::mpmc::BoundedChannelTestAccess::seed_empty_position(
        lastReservation, kPositionMask - 1U);
    int value = 91;
    if (!lastReservation.try_send(std::move(value)) ||
        lastReservation.is_closed() ||
        galay::mpmc::BoundedChannelTestAccess::tail_position(lastReservation) !=
            kPositionMask) {
        return false;
    }
    lastReservation.close();
    auto received = lastReservation.try_recv();
    if (!lastReservation.is_closed() || !received.has_value() || *received != 91 ||
        !lastReservation.empty()) {
        return false;
    }

    galay::mpmc::BoundedChannel<int> exhausted(2);
    galay::mpmc::BoundedChannelTestAccess::seed_empty_position(exhausted,
                                                            kPositionMask);
    int rejected = 92;
    return !exhausted.try_send(std::move(rejected)) && exhausted.is_closed() &&
        galay::mpmc::BoundedChannelTestAccess::tail_position(exhausted) ==
            kPositionMask;
}

}  // namespace

int main()
{
    if (!run_recv_immediate<false>(true) || !run_recv_immediate<false>(false) ||
        !run_recv_producer_first<false>() || !run_recv_timer_first<false>() ||
        !run_recv_close_first<false>() || !run_recv_timer_then_close<false>()) {
        std::cerr << "[T162] MPMC bounded recv timeout arbitration failed\n";
        return 1;
    }
    if (!run_recv_immediate<true>(true) || !run_recv_immediate<true>(false) ||
        !run_recv_producer_first<true>() || !run_recv_timer_first<true>() ||
        !run_recv_close_first<true>() || !run_recv_timer_then_close<true>()) {
        std::cerr << "[T162] MPMC bounded recvBatch timeout arbitration failed\n";
        return 1;
    }
    if (!run_send_immediate(true) || !run_send_immediate(false) ||
        !run_send_consumer_first() || !run_send_timer_first() ||
        !run_send_close_first() || !run_send_timer_then_close()) {
        std::cerr << "[T162] MPMC bounded send timeout arbitration failed\n";
        return 1;
    }
    if (!run_timed_out_recv_does_not_swallow_message<false>() ||
        !run_timed_out_recv_does_not_swallow_message<true>() ||
        !run_timed_out_send_does_not_swallow_slot()) {
        std::cerr << "[T162] timed-out waiter swallowed a resource event\n";
        return 1;
    }
    if (!run_recv_pump_skips_cancelled_waiter()) {
        std::cerr << "[T162] recv pump stopped at a cancelled waiter\n";
        return 1;
    }
    if (!run_close_waits_for_in_flight_send()) {
        std::cerr << "[T162] close overtook an in-flight send publication\n";
        return 1;
    }
    if (!run_tail_close_bit_boundary()) {
        std::cerr << "[T162] tail close-bit boundary protocol failed\n";
        return 1;
    }

    std::cout << "T162-MpmcBoundedTimeoutRace PASS\n";
    return 0;
}
