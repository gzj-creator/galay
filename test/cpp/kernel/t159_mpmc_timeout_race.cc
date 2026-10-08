/**
 * @file t159_mpmc_timeout_race.cc
 * @brief 验证 MPMC unbounded recv timeout 与 producer 只产生一个完成者。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/unbounded_channel.h>
#include "test/cpp/common/scheduler_test_adapter.h"
#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace galay::mpmc {

struct UnboundedChannelTestAccess
{
    template <UnboundedValue T>
    inline static thread_local typename UnboundedChannel<T>::Block* heldBlock = nullptr;

    template <UnboundedValue T>
    inline static thread_local uint64_t heldPosition = 0;

    template <UnboundedValue T>
    static bool hold_recv_pump(UnboundedChannel<T>& channel) noexcept
    {
        uint8_t expected = 0;
        return channel.m_recvPumpState.compare_exchange_strong(
            expected,
            UnboundedChannel<T>::kPumpRunning,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    template <UnboundedValue T>
    static bool recv_work_pending(const UnboundedChannel<T>& channel) noexcept
    {
        return (channel.m_recvPumpState.load(std::memory_order_acquire) &
                UnboundedChannel<T>::kRecvWork) != 0;
    }

    template <UnboundedValue T>
    static bool recv_pump_idle(const UnboundedChannel<T>& channel) noexcept
    {
        return channel.m_recvPumpState.load(std::memory_order_acquire) == 0;
    }

    template <UnboundedValue T>
    static size_t recv_waiter_count(const UnboundedChannel<T>& channel) noexcept
    {
        return channel.m_recvWaiters.size_approx();
    }

    template <UnboundedValue T>
    static void run_held_recv_pump(UnboundedChannel<T>& channel) noexcept
    {
        channel.run_recv_pump();
    }

    template <UnboundedValue T>
    static bool acquire_held_send(
        UnboundedChannel<T>& channel,
        typename UnboundedChannel<T>::ProducerToken& token) noexcept
    {
        if (!token.valid_for(channel) || heldBlock<T> != nullptr) {
            return false;
        }
        return channel.reserve_range(&token, 1, heldBlock<T>, heldPosition<T>);
    }

    template <UnboundedValue T>
    static bool enqueue_held_send(
        UnboundedChannel<T>& channel,
        typename UnboundedChannel<T>::ProducerToken& token,
        T&& value)
    {
        if (!token.valid_for(channel) || heldBlock<T> == nullptr) {
            return false;
        }
        auto& block = *heldBlock<T>;
        auto& slot = block.slots[static_cast<size_t>(
            heldPosition<T> & (UnboundedChannel<T>::kSlotsPerBlock - 1))];
        std::construct_at(slot.value(), std::move(value));
        slot.sequence.store(heldPosition<T> + 1, std::memory_order_release);
        channel.release_send_publication();
        return true;
    }

    template <UnboundedValue T>
    static void release_held_send(
        UnboundedChannel<T>&,
        typename UnboundedChannel<T>::ProducerToken&) noexcept
    {
        heldBlock<T> = nullptr;
        heldPosition<T> = 0;
    }

    template <UnboundedValue T>
    static bool retry_recv_before_closed(UnboundedChannel<T>& channel, T& value)
    {
        return channel.probe_recv_after_empty(value) ==
            UnboundedChannel<T>::ClosedRecvProbe::kValue;
    }

    template <UnboundedValue T>
    static bool send_side_quiescent_after_close(
        const UnboundedChannel<T>& channel) noexcept
    {
        return channel.send_side_quiescent_after_close();
    }

    template <UnboundedValue T>
    static bool closed_after_empty(UnboundedChannel<T>& channel, T& value)
    {
        return channel.probe_recv_after_empty(value) ==
            UnboundedChannel<T>::ClosedRecvProbe::kClosed;
    }
};

} // namespace galay::mpmc

namespace {

using namespace galay::kernel;
using namespace std::chrono_literals;

struct ReceiveState
{
    std::atomic<int> resumeCount{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> receivedValue{false};
    std::atomic<bool> receivedTimeout{false};
};

struct SendMoveGate
{
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

struct LifecycleValue
{
    SendMoveGate* moveGate = nullptr;
    int value = 0;

    LifecycleValue() noexcept = default;

    LifecycleValue(int initialValue, SendMoveGate* gate) noexcept
        : moveGate(gate), value(initialValue)
    {
    }

    LifecycleValue(const LifecycleValue&) = delete;
    LifecycleValue& operator=(const LifecycleValue&) = delete;

    LifecycleValue(LifecycleValue&& other) noexcept
        : moveGate(nullptr), value(other.value)
    {
        SendMoveGate* gate = std::exchange(other.moveGate, nullptr);
        if (gate != nullptr) {
            gate->entered.store(true, std::memory_order_release);
            while (!gate->release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    }

    LifecycleValue& operator=(LifecycleValue&& other) noexcept
    {
        if (this != &other) {
            moveGate = nullptr;
            value = other.value;
            other.moveGate = nullptr;
        }
        return *this;
    }
};

struct LifecycleReceiveState
{
    std::atomic<int> resumeCount{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> receivedValue{false};
    std::atomic<bool> receivedClosed{false};
};

class ImmediateRaceScheduler final : public detail::SchedulerTestAdapter<ImmediateRaceScheduler>
{
public:
    ImmediateRaceScheduler(galay::mpmc::UnboundedChannel<int>* channel,
                           bool publishValue) noexcept
        : m_channel(channel), m_publishValue(publishValue)
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
        if (m_publishValue) {
            m_sendSucceeded.store(m_channel->send(73), std::memory_order_release);
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

    bool send_succeeded() const noexcept
    {
        return m_sendSucceeded.load(std::memory_order_acquire);
    }

private:
    galay::mpmc::UnboundedChannel<int>* m_channel;
    const bool m_publishValue;
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
Task<void> receive_with_timeout(galay::mpmc::UnboundedChannel<int>* channel,
                              ReceiveState* state)
{
    if constexpr (Batch) {
        auto result = co_await channel->recv_batch(4).timeout(1h);
        state->resumeCount.fetch_add(1, std::memory_order_relaxed);
        if (result.has_value()) {
            state->receivedValue.store(
                result->size() == 1 && result->front() == 73,
                std::memory_order_release);
        } else {
            state->receivedTimeout.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
        }
    } else {
        auto result = co_await channel->recv().timeout(1h);
        state->resumeCount.fetch_add(1, std::memory_order_relaxed);
        if (result.has_value()) {
            state->receivedValue.store(*result == 73, std::memory_order_release);
        } else {
            state->receivedTimeout.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
        }
    }
    state->completed.store(true, std::memory_order_release);
    co_return;
}

Task<void> receive_lifecycle_value(
    galay::mpmc::UnboundedChannel<LifecycleValue>* channel,
    LifecycleReceiveState* state)
{
    auto result = co_await channel->recv();
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    if (result.has_value()) {
        state->receivedValue.store(result->value == 73, std::memory_order_release);
    } else {
        state->receivedClosed.store(
            IOError::contains(result.error().code(), kClosed),
            std::memory_order_release);
    }
    state->completed.store(true, std::memory_order_release);
    co_return;
}

template <bool Batch>
bool run_immediate_completion(bool publishValue)
{
    galay::mpmc::UnboundedChannel<int> channel;
    ImmediateRaceScheduler scheduler(&channel, publishValue);
    ReceiveState state;

    auto task = receive_with_timeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool correctResult = publishValue
        ? state.receivedValue.load(std::memory_order_acquire) &&
            !state.receivedTimeout.load(std::memory_order_acquire) &&
            scheduler.send_succeeded()
        : !state.receivedValue.load(std::memory_order_acquire) &&
            state.receivedTimeout.load(std::memory_order_acquire);

    const bool passed = started && state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 && correctResult &&
        scheduler.add_timer_calls() == 1 && scheduler.schedule_calls() == 0 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
    if (!passed) {
        std::cerr << "immediate publish=" << publishValue
                  << " started=" << started
                  << " completed=" << state.completed.load(std::memory_order_relaxed)
                  << " resumes=" << state.resumeCount.load(std::memory_order_relaxed)
                  << " value=" << state.receivedValue.load(std::memory_order_relaxed)
                  << " timeout=" << state.receivedTimeout.load(std::memory_order_relaxed)
                  << " add_timer=" << scheduler.add_timer_calls()
                  << " schedules=" << scheduler.schedule_calls()
                  << " refs=" << taskState->m_refs.load(std::memory_order_relaxed)
                  << " empty=" << channel.empty() << '\n';
    }
    return passed;
}

template <bool Batch>
bool run_producer_timer_arbitration()
{
    galay::mpmc::UnboundedChannel<int> channel;
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receive_with_timeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool sent = channel.send(73);
    const bool oneProducerWake = scheduler.has_single_ready_task();
    const bool noDuplicateWake = scheduler.resume_with_timer_in_dequeue_gap();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedValue.load(std::memory_order_acquire) &&
        !state.receivedTimeout.load(std::memory_order_acquire);

    scheduler.release_retained_state();
    return started && sent && oneProducerWake && noDuplicateWake && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool run_timer_producer_arbitration()
{
    galay::mpmc::UnboundedChannel<int> channel;
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receive_with_timeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool timerFired = scheduler.fire_timer();
    const bool oneTimerWake = scheduler.has_single_ready_task();
    const bool sent = channel.send(73);
    const bool stillOneWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    const bool timedOut = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        !state.receivedValue.load(std::memory_order_acquire) &&
        state.receivedTimeout.load(std::memory_order_acquire);
    auto retainedValue = channel.try_recv();

    scheduler.release_retained_state();
    return started && timerFired && oneTimerWake && sent && stillOneWake &&
        resumed && timedOut && retainedValue.has_value() && *retainedValue == 73 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool run_timed_out_waiter_does_not_swallow_message()
{
    galay::mpmc::UnboundedChannel<int> channel;
    QueuedRaceScheduler scheduler;
    ReceiveState timedOutState;
    ReceiveState liveState;

    auto liveTask = receive_with_timeout<Batch>(&channel, &liveState);
    auto timedOutTask = receive_with_timeout<Batch>(&channel, &timedOutState);
    TaskRef timedOutKeeper = detail::TaskAccess::task_ref(timedOutTask);
    TaskRef liveKeeper = detail::TaskAccess::task_ref(liveTask);
    TaskState* timedOutTaskState = timedOutKeeper.state();
    TaskState* liveTaskState = liveKeeper.state();
    if (timedOutTaskState == nullptr || liveTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled = detail::TaskAccess::detach_task(std::move(liveTask));
    TaskRef secondScheduled = detail::TaskAccess::detach_task(std::move(timedOutTask));
    const bool firstStarted = scheduler.schedule_immediately(std::move(firstScheduled));
    const bool secondStarted = scheduler.schedule_immediately(std::move(secondScheduled));
    const bool timerFired = scheduler.fire_timer(1);
    const bool timeoutReady = scheduler.ready_count() == 1;
    const bool sent = channel.send(73);
    const bool bothReady = scheduler.ready_count() == 2;
    const bool firstResumed = scheduler.run_one();
    const bool secondResumed = scheduler.run_one();
    const bool timedOut = timedOutState.completed.load(std::memory_order_acquire) &&
        timedOutState.resumeCount.load(std::memory_order_acquire) == 1 &&
        !timedOutState.receivedValue.load(std::memory_order_acquire) &&
        timedOutState.receivedTimeout.load(std::memory_order_acquire);
    const bool received = liveState.completed.load(std::memory_order_acquire) &&
        liveState.resumeCount.load(std::memory_order_acquire) == 1 &&
        liveState.receivedValue.load(std::memory_order_acquire) &&
        !liveState.receivedTimeout.load(std::memory_order_acquire);

    scheduler.release_retained_state();
    return firstStarted && secondStarted && timerFired && timeoutReady && sent &&
        bothReady && firstResumed && secondResumed && timedOut && received &&
        timedOutTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        liveTaskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool run_pump_retains_work_while_owned()
{
    galay::mpmc::UnboundedChannel<int> channel;
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    if (!galay::mpmc::UnboundedChannelTestAccess::hold_recv_pump(channel)) {
        return false;
    }

    auto task = receive_with_timeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool registrationRetained =
        galay::mpmc::UnboundedChannelTestAccess::recv_work_pending(channel);
    const bool sent = channel.send(73);
    const bool sendRetained =
        galay::mpmc::UnboundedChannelTestAccess::recv_work_pending(channel);

    galay::mpmc::UnboundedChannelTestAccess::run_held_recv_pump(channel);
    const bool oneWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedValue.load(std::memory_order_acquire) &&
        !state.receivedTimeout.load(std::memory_order_acquire);
    const bool pumpIdle =
        galay::mpmc::UnboundedChannelTestAccess::recv_pump_idle(channel);

    scheduler.release_retained_state();
    return started && registrationRetained && sent && sendRetained && oneWake &&
        resumed && completed && pumpIdle &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool run_timeout_cleanup_behind_live_waiter()
{
    galay::mpmc::UnboundedChannel<int> channel;
    QueuedRaceScheduler scheduler;
    ReceiveState liveState;
    ReceiveState timedOutState;

    auto timedOutTask = receive_with_timeout<Batch>(&channel, &timedOutState);
    auto liveTask = receive_with_timeout<Batch>(&channel, &liveState);
    TaskRef liveKeeper = detail::TaskAccess::task_ref(liveTask);
    TaskRef timedOutKeeper = detail::TaskAccess::task_ref(timedOutTask);
    TaskState* liveTaskState = liveKeeper.state();
    TaskState* timedOutTaskState = timedOutKeeper.state();
    if (liveTaskState == nullptr || timedOutTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled = detail::TaskAccess::detach_task(std::move(timedOutTask));
    TaskRef secondScheduled = detail::TaskAccess::detach_task(std::move(liveTask));
    const bool firstStarted = scheduler.schedule_immediately(std::move(firstScheduled));
    const bool secondStarted = scheduler.schedule_immediately(std::move(secondScheduled));
    const bool timerFired = scheduler.fire_timer(0);
    const bool timeoutReady = scheduler.has_single_ready_task();
    const bool timeoutResumed = scheduler.run_one();
    const bool timedOut = timedOutState.completed.load(std::memory_order_acquire) &&
        timedOutState.resumeCount.load(std::memory_order_acquire) == 1 &&
        timedOutState.receivedTimeout.load(std::memory_order_acquire);
    const bool staleEntryRemoved =
        galay::mpmc::UnboundedChannelTestAccess::recv_waiter_count(channel) == 1;

    channel.close();
    const bool liveReady = scheduler.has_single_ready_task();
    const bool liveResumed = scheduler.run_one();
    const bool liveCompleted = liveState.completed.load(std::memory_order_acquire) &&
        liveState.resumeCount.load(std::memory_order_acquire) == 1;

    scheduler.release_retained_state();
    return firstStarted && secondStarted && timerFired && timeoutReady &&
        timeoutResumed && timedOut && staleEntryRemoved && liveReady && liveResumed &&
        liveCompleted && liveTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        timedOutTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

template <bool UseToken>
bool run_close_waits_for_in_flight_send_before_enqueue()
{
    galay::mpmc::UnboundedChannel<LifecycleValue> channel;
    QueuedRaceScheduler scheduler;
    LifecycleReceiveState state;
    SendMoveGate moveGate;

    auto task = receive_lifecycle_value(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));

    std::atomic<bool> sendResult{false};
    std::thread producer([&]() {
        if constexpr (UseToken) {
            auto token = channel.make_producer_token();
            sendResult.store(
                token.valid() &&
                    channel.send(token, LifecycleValue(73, &moveGate)),
                std::memory_order_release);
        } else {
            sendResult.store(channel.send(LifecycleValue(73, &moveGate)),
                             std::memory_order_release);
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!moveGate.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    channel.close();
    const bool lateSendRejected = !channel.send(LifecycleValue(99, nullptr));
    const bool noEarlyClosedWake = scheduler.ready_count() == 0;
    moveGate.release.store(true, std::memory_order_release);
    producer.join();

    const bool oneValueWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedValue.load(std::memory_order_acquire) &&
        !state.receivedClosed.load(std::memory_order_acquire);

    scheduler.release_retained_state();
    return started && moveGate.entered.load(std::memory_order_acquire) &&
        lateSendRejected && noEarlyClosedWake &&
        sendResult.load(std::memory_order_acquire) && oneValueWake && resumed &&
        completed && channel.empty() &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_close_waits_for_in_flight_send_after_enqueue()
{
    galay::mpmc::UnboundedChannel<LifecycleValue> channel;
    QueuedRaceScheduler scheduler;
    LifecycleReceiveState firstState;
    LifecycleReceiveState secondState;

    auto firstTask = receive_lifecycle_value(&channel, &firstState);
    auto secondTask = receive_lifecycle_value(&channel, &secondState);
    TaskRef firstKeeper = detail::TaskAccess::task_ref(firstTask);
    TaskRef secondKeeper = detail::TaskAccess::task_ref(secondTask);
    TaskState* firstTaskState = firstKeeper.state();
    TaskState* secondTaskState = secondKeeper.state();
    if (firstTaskState == nullptr || secondTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled = detail::TaskAccess::detach_task(std::move(firstTask));
    TaskRef secondScheduled = detail::TaskAccess::detach_task(std::move(secondTask));
    const bool firstStarted = scheduler.schedule_immediately(std::move(firstScheduled));
    const bool secondStarted = scheduler.schedule_immediately(std::move(secondScheduled));
    auto producerToken = channel.make_producer_token();
    const bool permitAcquired =
        producerToken.valid() &&
        galay::mpmc::UnboundedChannelTestAccess::acquire_held_send(
            channel, producerToken);
    const bool enqueued = permitAcquired &&
        galay::mpmc::UnboundedChannelTestAccess::enqueue_held_send(
            channel, producerToken, LifecycleValue(73, nullptr));

    channel.close();
    const bool valueAndClosedReady = scheduler.ready_count() == 2;
    const bool valueResumed = scheduler.run_one();
    const int valuesBeforeRelease =
        static_cast<int>(firstState.receivedValue.load(std::memory_order_acquire)) +
        static_cast<int>(secondState.receivedValue.load(std::memory_order_acquire));
    const bool closedReady = scheduler.has_single_ready_task();

    if (permitAcquired) {
        galay::mpmc::UnboundedChannelTestAccess::release_held_send(
            channel, producerToken);
    }
    const bool closedResumed = scheduler.run_one();
    const int closedAfterRelease =
        static_cast<int>(firstState.receivedClosed.load(std::memory_order_acquire)) +
        static_cast<int>(secondState.receivedClosed.load(std::memory_order_acquire));

    scheduler.release_retained_state();
    return firstStarted && secondStarted && permitAcquired && enqueued &&
        valueAndClosedReady && valueResumed && valuesBeforeRelease == 1 &&
        closedReady && closedResumed && closedAfterRelease == 1 &&
        firstState.resumeCount.load(std::memory_order_acquire) == 1 &&
        secondState.resumeCount.load(std::memory_order_acquire) == 1 &&
        firstTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        secondTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        channel.empty();
}

bool run_completed_send_between_empty_check_and_producer_scan()
{
    galay::mpmc::UnboundedChannel<int> channel;
    auto producerToken = channel.make_producer_token();
    if (!producerToken.valid() ||
        !galay::mpmc::UnboundedChannelTestAccess::acquire_held_send(
            channel, producerToken)) {
        return false;
    }

    const bool firstCheckEmpty = !channel.try_recv().has_value();
    channel.close();
    const bool enqueued =
        galay::mpmc::UnboundedChannelTestAccess::enqueue_held_send(
            channel, producerToken, 73);
    galay::mpmc::UnboundedChannelTestAccess::release_held_send(
        channel, producerToken);

    int value = 0;
    const bool retriedValue =
        galay::mpmc::UnboundedChannelTestAccess::retry_recv_before_closed(
            channel, value);
    return firstCheckEmpty && enqueued && retriedValue && value == 73 &&
        channel.empty();
}

bool run_close_scan_across_repeated_send_cycle()
{
    galay::mpmc::UnboundedChannel<int> channel;
    auto producerToken = channel.make_producer_token();
    if (!producerToken.valid() || !channel.send(producerToken, 41)) {
        return false;
    }

    auto firstValue = channel.try_recv();
    if (!firstValue.has_value() || *firstValue != 41 || !channel.empty()) {
        return false;
    }
    if (!galay::mpmc::UnboundedChannelTestAccess::acquire_held_send(
            channel, producerToken)) {
        return false;
    }

    channel.close();
    int value = 0;
    const bool activeObserved =
        !galay::mpmc::UnboundedChannelTestAccess::send_side_quiescent_after_close(
            channel);
    const bool noEarlyClosed =
        !galay::mpmc::UnboundedChannelTestAccess::closed_after_empty(channel, value);

    const bool publishedAfterClose =
        galay::mpmc::UnboundedChannelTestAccess::enqueue_held_send(
            channel, producerToken, 42);
    galay::mpmc::UnboundedChannelTestAccess::release_held_send(
        channel, producerToken);
    const bool secondPermitRejected =
        !galay::mpmc::UnboundedChannelTestAccess::acquire_held_send(
            channel, producerToken);
    const bool completedValue =
        galay::mpmc::UnboundedChannelTestAccess::retry_recv_before_closed(
            channel, value) && value == 42;
    const bool idleObserved =
        galay::mpmc::UnboundedChannelTestAccess::send_side_quiescent_after_close(
            channel);
    const bool closedAfterRelease =
        galay::mpmc::UnboundedChannelTestAccess::closed_after_empty(channel, value);
    return activeObserved && noEarlyClosed && publishedAfterClose &&
        secondPermitRejected && completedValue && idleObserved &&
        closedAfterRelease && channel.empty();
}

}  // namespace

int main()
{
    if (!run_immediate_completion<false>(true)) {
        std::cerr << "[T159] recv operation-win immediate failed\n";
        return 1;
    }
    if (!run_immediate_completion<false>(false)) {
        std::cerr << "[T159] recv timeout-win immediate failed\n";
        return 1;
    }
    if (!run_producer_timer_arbitration<false>()) {
        std::cerr << "[T159] recv producer-first gap failed\n";
        return 1;
    }
    if (!run_timer_producer_arbitration<false>()) {
        std::cerr << "[T159] recv timer-first retention failed\n";
        return 1;
    }
    if (!run_immediate_completion<true>(true)) {
        std::cerr << "[T159] recvBatch operation-win immediate failed\n";
        return 1;
    }
    if (!run_immediate_completion<true>(false)) {
        std::cerr << "[T159] recvBatch timeout-win immediate failed\n";
        return 1;
    }
    if (!run_producer_timer_arbitration<true>()) {
        std::cerr << "[T159] recvBatch producer-first gap failed\n";
        return 1;
    }
    if (!run_timer_producer_arbitration<true>()) {
        std::cerr << "[T159] recvBatch timer-first retention failed\n";
        return 1;
    }
    if (!run_timed_out_waiter_does_not_swallow_message<false>() ||
        !run_timed_out_waiter_does_not_swallow_message<true>()) {
        std::cerr << "[T159] timed-out waiter swallowed a message event\n";
        return 1;
    }
    if (!run_pump_retains_work_while_owned<false>() ||
        !run_pump_retains_work_while_owned<true>()) {
        std::cerr << "[T159] recv pump lost work while owned\n";
        return 1;
    }
    if (!run_timeout_cleanup_behind_live_waiter<false>() ||
        !run_timeout_cleanup_behind_live_waiter<true>()) {
        std::cerr << "[T159] timeout tombstone remained behind live waiter\n";
        return 1;
    }
    if (!run_close_waits_for_in_flight_send_before_enqueue<false>() ||
        !run_close_waits_for_in_flight_send_before_enqueue<true>()) {
        std::cerr << "[T159] close overtook in-flight send before enqueue\n";
        return 1;
    }
    if (!run_close_waits_for_in_flight_send_after_enqueue()) {
        std::cerr << "[T159] close overtook in-flight send after enqueue\n";
        return 1;
    }
    if (!run_completed_send_between_empty_check_and_producer_scan()) {
        std::cerr << "[T159] close skipped second dequeue after producer scan\n";
        return 1;
    }
    if (!run_close_scan_across_repeated_send_cycle()) {
        std::cerr << "[T159] close reused stale idle state across send cycles\n";
        return 1;
    }

    std::cout << "T159-MpmcTimeoutRace PASS\n";
    return 0;
}
