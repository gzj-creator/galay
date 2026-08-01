/**
 * @file t159_mpmc_timeout_race.cc
 * @brief 验证 MPMC unbounded recv timeout 与 producer 只产生一个完成者。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/core/scheduler.hpp>
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
    static bool holdRecvPump(UnboundedChannel<T>& channel) noexcept
    {
        uint8_t expected = 0;
        return channel.m_recvPumpState.compare_exchange_strong(
            expected,
            UnboundedChannel<T>::kPumpRunning,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    template <UnboundedValue T>
    static bool recvWorkPending(const UnboundedChannel<T>& channel) noexcept
    {
        return (channel.m_recvPumpState.load(std::memory_order_acquire) &
                UnboundedChannel<T>::kRecvWork) != 0;
    }

    template <UnboundedValue T>
    static bool recvPumpIdle(const UnboundedChannel<T>& channel) noexcept
    {
        return channel.m_recvPumpState.load(std::memory_order_acquire) == 0;
    }

    template <UnboundedValue T>
    static size_t recvWaiterCount(const UnboundedChannel<T>& channel) noexcept
    {
        return channel.m_recvWaiters.size_approx();
    }

    template <UnboundedValue T>
    static void runHeldRecvPump(UnboundedChannel<T>& channel) noexcept
    {
        channel.runRecvPump();
    }

    template <UnboundedValue T>
    static bool acquireHeldSend(
        UnboundedChannel<T>& channel,
        typename UnboundedChannel<T>::ProducerToken& token) noexcept
    {
        return token.validFor(channel) &&
            channel.acquireSendPermit(*token.m_epochNode);
    }

    template <UnboundedValue T>
    static bool enqueueHeldSend(
        UnboundedChannel<T>& channel,
        typename UnboundedChannel<T>::ProducerToken& token,
        T&& value)
    {
        if (!channel.m_queue.enqueue(
                token.m_epochNode->queueToken, std::move(value))) {
            return false;
        }
        if (channel.waiterPathUsedAfterPublish()) {
            channel.requestRecvPump();
        }
        return true;
    }

    template <UnboundedValue T>
    static void releaseHeldSend(
        UnboundedChannel<T>& channel,
        typename UnboundedChannel<T>::ProducerToken& token) noexcept
    {
        channel.releaseSendPermit(*token.m_epochNode);
    }

    template <UnboundedValue T>
    static bool retryRecvBeforeClosed(UnboundedChannel<T>& channel, T& value)
    {
        return channel.probeRecvAfterEmpty(value) ==
            UnboundedChannel<T>::ClosedRecvProbe::kValue;
    }

    template <UnboundedValue T>
    static bool sendSideQuiescentAfterClose(
        const UnboundedChannel<T>& channel) noexcept
    {
        return channel.sendSideQuiescentAfterClose();
    }

    template <UnboundedValue T>
    static bool closedAfterEmpty(UnboundedChannel<T>& channel, T& value)
    {
        return channel.probeRecvAfterEmpty(value) ==
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

class ImmediateRaceScheduler final : public Scheduler
{
public:
    ImmediateRaceScheduler(galay::mpmc::UnboundedChannel<int>* channel,
                           bool publishValue) noexcept
        : m_channel(channel), m_publishValue(publishValue)
    {
    }

    std::expected<void, IOError> start() override { return {}; }
    void stop() override {}

    bool schedule(TaskRef task) noexcept override
    {
        if (!bindTask(task)) {
            return false;
        }
        m_scheduleCalls.fetch_add(1, std::memory_order_relaxed);
        resume(task);
        return true;
    }

    bool scheduleResume(TaskRef task) noexcept override
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
        m_addTimerCalls.fetch_add(1, std::memory_order_relaxed);
        if (m_publishValue) {
            m_sendSucceeded.store(m_channel->send(73), std::memory_order_release);
        }
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
    galay::mpmc::UnboundedChannel<int>* m_channel;
    const bool m_publishValue;
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

    bool scheduleResume(TaskRef task) noexcept override
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
        m_timers.push_back(std::move(timer));
        return true;
    }

    SchedulerType type() override { return kComputeScheduler; }

    bool hasSingleReadyTask() const noexcept { return m_ready.size() == 1; }

    size_t readyCount() const noexcept { return m_ready.size(); }

    bool fireTimer()
    {
        return fireTimer(0);
    }

    bool fireTimer(size_t index)
    {
        if (index >= m_timers.size() || !m_timers[index]) {
            return false;
        }
        m_timers[index]->handleTimeout();
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
        if (m_ready.empty() || m_timers.empty() || !m_timers.front()) {
            return false;
        }
        TaskRef task = std::move(m_ready.front());
        m_ready.pop_front();
        TaskState* state = task.state();
        if (state == nullptr || !state->m_handle ||
            state->m_done.load(std::memory_order_acquire)) {
            return false;
        }

        state->m_queued.store(false, std::memory_order_relaxed);
        state->m_resume_owner_only.store(false, std::memory_order_relaxed);
        m_timers.front()->handleTimeout();
        const bool noDuplicateWake = m_ready.empty();
        state->m_handle.resume();
        return noDuplicateWake;
    }

    void releaseRetainedState()
    {
        m_ready.clear();
        m_timers.clear();
    }

private:
    std::deque<TaskRef> m_ready;
    std::vector<Timer::ptr> m_timers;
};

template <bool Batch>
Task<void> receiveWithTimeout(galay::mpmc::UnboundedChannel<int>* channel,
                              ReceiveState* state)
{
    if constexpr (Batch) {
        auto result = co_await channel->recvBatch(4).timeout(1h);
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

Task<void> receiveLifecycleValue(
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
bool runImmediateCompletion(bool publishValue)
{
    galay::mpmc::UnboundedChannel<int> channel;
    ImmediateRaceScheduler scheduler(&channel, publishValue);
    ReceiveState state;

    auto task = receiveWithTimeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool correctResult = publishValue
        ? state.receivedValue.load(std::memory_order_acquire) &&
            !state.receivedTimeout.load(std::memory_order_acquire) &&
            scheduler.sendSucceeded()
        : !state.receivedValue.load(std::memory_order_acquire) &&
            state.receivedTimeout.load(std::memory_order_acquire);

    const bool passed = started && state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 && correctResult &&
        scheduler.addTimerCalls() == 1 && scheduler.scheduleCalls() == 0 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
    if (!passed) {
        std::cerr << "immediate publish=" << publishValue
                  << " started=" << started
                  << " completed=" << state.completed.load(std::memory_order_relaxed)
                  << " resumes=" << state.resumeCount.load(std::memory_order_relaxed)
                  << " value=" << state.receivedValue.load(std::memory_order_relaxed)
                  << " timeout=" << state.receivedTimeout.load(std::memory_order_relaxed)
                  << " add_timer=" << scheduler.addTimerCalls()
                  << " schedules=" << scheduler.scheduleCalls()
                  << " refs=" << taskState->m_refs.load(std::memory_order_relaxed)
                  << " empty=" << channel.empty() << '\n';
    }
    return passed;
}

template <bool Batch>
bool runProducerTimerArbitration()
{
    galay::mpmc::UnboundedChannel<int> channel;
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receiveWithTimeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool sent = channel.send(73);
    const bool oneProducerWake = scheduler.hasSingleReadyTask();
    const bool noDuplicateWake = scheduler.resumeWithTimerInDequeueGap();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedValue.load(std::memory_order_acquire) &&
        !state.receivedTimeout.load(std::memory_order_acquire);

    scheduler.releaseRetainedState();
    return started && sent && oneProducerWake && noDuplicateWake && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool runTimerProducerArbitration()
{
    galay::mpmc::UnboundedChannel<int> channel;
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receiveWithTimeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool timerFired = scheduler.fireTimer();
    const bool oneTimerWake = scheduler.hasSingleReadyTask();
    const bool sent = channel.send(73);
    const bool stillOneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    const bool timedOut = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        !state.receivedValue.load(std::memory_order_acquire) &&
        state.receivedTimeout.load(std::memory_order_acquire);
    auto retainedValue = channel.tryRecv();

    scheduler.releaseRetainedState();
    return started && timerFired && oneTimerWake && sent && stillOneWake &&
        resumed && timedOut && retainedValue.has_value() && *retainedValue == 73 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool runTimedOutWaiterDoesNotSwallowMessage()
{
    galay::mpmc::UnboundedChannel<int> channel;
    QueuedRaceScheduler scheduler;
    ReceiveState timedOutState;
    ReceiveState liveState;

    auto liveTask = receiveWithTimeout<Batch>(&channel, &liveState);
    auto timedOutTask = receiveWithTimeout<Batch>(&channel, &timedOutState);
    TaskRef timedOutKeeper = detail::TaskAccess::taskRef(timedOutTask);
    TaskRef liveKeeper = detail::TaskAccess::taskRef(liveTask);
    TaskState* timedOutTaskState = timedOutKeeper.state();
    TaskState* liveTaskState = liveKeeper.state();
    if (timedOutTaskState == nullptr || liveTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled = detail::TaskAccess::detachTask(std::move(liveTask));
    TaskRef secondScheduled = detail::TaskAccess::detachTask(std::move(timedOutTask));
    const bool firstStarted = scheduler.scheduleImmediately(std::move(firstScheduled));
    const bool secondStarted = scheduler.scheduleImmediately(std::move(secondScheduled));
    const bool timerFired = scheduler.fireTimer(1);
    const bool timeoutReady = scheduler.readyCount() == 1;
    const bool sent = channel.send(73);
    const bool bothReady = scheduler.readyCount() == 2;
    const bool firstResumed = scheduler.runOne();
    const bool secondResumed = scheduler.runOne();
    const bool timedOut = timedOutState.completed.load(std::memory_order_acquire) &&
        timedOutState.resumeCount.load(std::memory_order_acquire) == 1 &&
        !timedOutState.receivedValue.load(std::memory_order_acquire) &&
        timedOutState.receivedTimeout.load(std::memory_order_acquire);
    const bool received = liveState.completed.load(std::memory_order_acquire) &&
        liveState.resumeCount.load(std::memory_order_acquire) == 1 &&
        liveState.receivedValue.load(std::memory_order_acquire) &&
        !liveState.receivedTimeout.load(std::memory_order_acquire);

    scheduler.releaseRetainedState();
    return firstStarted && secondStarted && timerFired && timeoutReady && sent &&
        bothReady && firstResumed && secondResumed && timedOut && received &&
        timedOutTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        liveTaskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool runPumpRetainsWorkWhileOwned()
{
    galay::mpmc::UnboundedChannel<int> channel;
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    if (!galay::mpmc::UnboundedChannelTestAccess::holdRecvPump(channel)) {
        return false;
    }

    auto task = receiveWithTimeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool registrationRetained =
        galay::mpmc::UnboundedChannelTestAccess::recvWorkPending(channel);
    const bool sent = channel.send(73);
    const bool sendRetained =
        galay::mpmc::UnboundedChannelTestAccess::recvWorkPending(channel);

    galay::mpmc::UnboundedChannelTestAccess::runHeldRecvPump(channel);
    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedValue.load(std::memory_order_acquire) &&
        !state.receivedTimeout.load(std::memory_order_acquire);
    const bool pumpIdle =
        galay::mpmc::UnboundedChannelTestAccess::recvPumpIdle(channel);

    scheduler.releaseRetainedState();
    return started && registrationRetained && sent && sendRetained && oneWake &&
        resumed && completed && pumpIdle &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool runTimeoutCleanupBehindLiveWaiter()
{
    galay::mpmc::UnboundedChannel<int> channel;
    QueuedRaceScheduler scheduler;
    ReceiveState liveState;
    ReceiveState timedOutState;

    auto timedOutTask = receiveWithTimeout<Batch>(&channel, &timedOutState);
    auto liveTask = receiveWithTimeout<Batch>(&channel, &liveState);
    TaskRef liveKeeper = detail::TaskAccess::taskRef(liveTask);
    TaskRef timedOutKeeper = detail::TaskAccess::taskRef(timedOutTask);
    TaskState* liveTaskState = liveKeeper.state();
    TaskState* timedOutTaskState = timedOutKeeper.state();
    if (liveTaskState == nullptr || timedOutTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled = detail::TaskAccess::detachTask(std::move(timedOutTask));
    TaskRef secondScheduled = detail::TaskAccess::detachTask(std::move(liveTask));
    const bool firstStarted = scheduler.scheduleImmediately(std::move(firstScheduled));
    const bool secondStarted = scheduler.scheduleImmediately(std::move(secondScheduled));
    const bool timerFired = scheduler.fireTimer(0);
    const bool timeoutReady = scheduler.hasSingleReadyTask();
    const bool timeoutResumed = scheduler.runOne();
    const bool timedOut = timedOutState.completed.load(std::memory_order_acquire) &&
        timedOutState.resumeCount.load(std::memory_order_acquire) == 1 &&
        timedOutState.receivedTimeout.load(std::memory_order_acquire);
    const bool staleEntryRemoved =
        galay::mpmc::UnboundedChannelTestAccess::recvWaiterCount(channel) == 1;

    channel.close();
    const bool liveReady = scheduler.hasSingleReadyTask();
    const bool liveResumed = scheduler.runOne();
    const bool liveCompleted = liveState.completed.load(std::memory_order_acquire) &&
        liveState.resumeCount.load(std::memory_order_acquire) == 1;

    scheduler.releaseRetainedState();
    return firstStarted && secondStarted && timerFired && timeoutReady &&
        timeoutResumed && timedOut && staleEntryRemoved && liveReady && liveResumed &&
        liveCompleted && liveTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        timedOutTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

template <bool UseToken>
bool runCloseWaitsForInFlightSendBeforeEnqueue()
{
    galay::mpmc::UnboundedChannel<LifecycleValue> channel;
    QueuedRaceScheduler scheduler;
    LifecycleReceiveState state;
    SendMoveGate moveGate;

    auto task = receiveLifecycleValue(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));

    std::atomic<bool> sendResult{false};
    std::thread producer([&]() {
        if constexpr (UseToken) {
            auto token = channel.makeProducerToken();
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
    const bool noEarlyClosedWake = scheduler.readyCount() == 0;
    moveGate.release.store(true, std::memory_order_release);
    producer.join();

    const bool oneValueWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedValue.load(std::memory_order_acquire) &&
        !state.receivedClosed.load(std::memory_order_acquire);

    scheduler.releaseRetainedState();
    return started && moveGate.entered.load(std::memory_order_acquire) &&
        lateSendRejected && noEarlyClosedWake &&
        sendResult.load(std::memory_order_acquire) && oneValueWake && resumed &&
        completed && channel.empty() &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runCloseWaitsForInFlightSendAfterEnqueue()
{
    galay::mpmc::UnboundedChannel<LifecycleValue> channel;
    QueuedRaceScheduler scheduler;
    LifecycleReceiveState firstState;
    LifecycleReceiveState secondState;

    auto firstTask = receiveLifecycleValue(&channel, &firstState);
    auto secondTask = receiveLifecycleValue(&channel, &secondState);
    TaskRef firstKeeper = detail::TaskAccess::taskRef(firstTask);
    TaskRef secondKeeper = detail::TaskAccess::taskRef(secondTask);
    TaskState* firstTaskState = firstKeeper.state();
    TaskState* secondTaskState = secondKeeper.state();
    if (firstTaskState == nullptr || secondTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled = detail::TaskAccess::detachTask(std::move(firstTask));
    TaskRef secondScheduled = detail::TaskAccess::detachTask(std::move(secondTask));
    const bool firstStarted = scheduler.scheduleImmediately(std::move(firstScheduled));
    const bool secondStarted = scheduler.scheduleImmediately(std::move(secondScheduled));
    auto producerToken = channel.makeProducerToken();
    const bool permitAcquired =
        producerToken.valid() &&
        galay::mpmc::UnboundedChannelTestAccess::acquireHeldSend(
            channel, producerToken);
    const bool enqueued = permitAcquired &&
        galay::mpmc::UnboundedChannelTestAccess::enqueueHeldSend(
            channel, producerToken, LifecycleValue(73, nullptr));

    channel.close();
    const bool onlyValueReady = scheduler.readyCount() == 1;
    const bool valueResumed = scheduler.runOne();
    const int valuesBeforeRelease =
        static_cast<int>(firstState.receivedValue.load(std::memory_order_acquire)) +
        static_cast<int>(secondState.receivedValue.load(std::memory_order_acquire));
    const bool noEarlyClosedWake = scheduler.readyCount() == 0;

    if (permitAcquired) {
        galay::mpmc::UnboundedChannelTestAccess::releaseHeldSend(
            channel, producerToken);
    }
    const bool closedReady = scheduler.hasSingleReadyTask();
    const bool closedResumed = scheduler.runOne();
    const int closedAfterRelease =
        static_cast<int>(firstState.receivedClosed.load(std::memory_order_acquire)) +
        static_cast<int>(secondState.receivedClosed.load(std::memory_order_acquire));

    scheduler.releaseRetainedState();
    return firstStarted && secondStarted && permitAcquired && enqueued &&
        onlyValueReady && valueResumed && valuesBeforeRelease == 1 &&
        noEarlyClosedWake && closedReady && closedResumed && closedAfterRelease == 1 &&
        firstState.resumeCount.load(std::memory_order_acquire) == 1 &&
        secondState.resumeCount.load(std::memory_order_acquire) == 1 &&
        firstTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        secondTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        channel.empty();
}

bool runCompletedSendBetweenEmptyCheckAndProducerScan()
{
    galay::mpmc::UnboundedChannel<int> channel;
    auto producerToken = channel.makeProducerToken();
    if (!producerToken.valid() ||
        !galay::mpmc::UnboundedChannelTestAccess::acquireHeldSend(
            channel, producerToken)) {
        return false;
    }

    const bool firstCheckEmpty = !channel.tryRecv().has_value();
    channel.close();
    const bool enqueued =
        galay::mpmc::UnboundedChannelTestAccess::enqueueHeldSend(
            channel, producerToken, 73);
    galay::mpmc::UnboundedChannelTestAccess::releaseHeldSend(
        channel, producerToken);

    int value = 0;
    const bool retriedValue =
        galay::mpmc::UnboundedChannelTestAccess::retryRecvBeforeClosed(
            channel, value);
    return firstCheckEmpty && enqueued && retriedValue && value == 73 &&
        channel.empty();
}

bool runCloseScanAcrossRepeatedSendCycle()
{
    galay::mpmc::UnboundedChannel<int> channel;
    auto producerToken = channel.makeProducerToken();
    if (!producerToken.valid() || !channel.send(producerToken, 41)) {
        return false;
    }

    auto firstValue = channel.tryRecv();
    if (!firstValue.has_value() || *firstValue != 41 || !channel.empty()) {
        return false;
    }
    if (!galay::mpmc::UnboundedChannelTestAccess::acquireHeldSend(
            channel, producerToken)) {
        return false;
    }

    channel.close();
    int value = 0;
    const bool activeObserved =
        !galay::mpmc::UnboundedChannelTestAccess::sendSideQuiescentAfterClose(
            channel);
    const bool noEarlyClosed =
        !galay::mpmc::UnboundedChannelTestAccess::closedAfterEmpty(channel, value);

    galay::mpmc::UnboundedChannelTestAccess::releaseHeldSend(
        channel, producerToken);
    const bool idleObserved =
        galay::mpmc::UnboundedChannelTestAccess::sendSideQuiescentAfterClose(
            channel);
    const bool closedAfterRelease =
        galay::mpmc::UnboundedChannelTestAccess::closedAfterEmpty(channel, value);
    return activeObserved && noEarlyClosed && idleObserved && closedAfterRelease &&
        channel.empty();
}

}  // namespace

int main()
{
    if (!runImmediateCompletion<false>(true)) {
        std::cerr << "[T159] recv operation-win immediate failed\n";
        return 1;
    }
    if (!runImmediateCompletion<false>(false)) {
        std::cerr << "[T159] recv timeout-win immediate failed\n";
        return 1;
    }
    if (!runProducerTimerArbitration<false>()) {
        std::cerr << "[T159] recv producer-first gap failed\n";
        return 1;
    }
    if (!runTimerProducerArbitration<false>()) {
        std::cerr << "[T159] recv timer-first retention failed\n";
        return 1;
    }
    if (!runImmediateCompletion<true>(true)) {
        std::cerr << "[T159] recvBatch operation-win immediate failed\n";
        return 1;
    }
    if (!runImmediateCompletion<true>(false)) {
        std::cerr << "[T159] recvBatch timeout-win immediate failed\n";
        return 1;
    }
    if (!runProducerTimerArbitration<true>()) {
        std::cerr << "[T159] recvBatch producer-first gap failed\n";
        return 1;
    }
    if (!runTimerProducerArbitration<true>()) {
        std::cerr << "[T159] recvBatch timer-first retention failed\n";
        return 1;
    }
    if (!runTimedOutWaiterDoesNotSwallowMessage<false>() ||
        !runTimedOutWaiterDoesNotSwallowMessage<true>()) {
        std::cerr << "[T159] timed-out waiter swallowed a message event\n";
        return 1;
    }
    if (!runPumpRetainsWorkWhileOwned<false>() ||
        !runPumpRetainsWorkWhileOwned<true>()) {
        std::cerr << "[T159] recv pump lost work while owned\n";
        return 1;
    }
    if (!runTimeoutCleanupBehindLiveWaiter<false>() ||
        !runTimeoutCleanupBehindLiveWaiter<true>()) {
        std::cerr << "[T159] timeout tombstone remained behind live waiter\n";
        return 1;
    }
    if (!runCloseWaitsForInFlightSendBeforeEnqueue<false>() ||
        !runCloseWaitsForInFlightSendBeforeEnqueue<true>()) {
        std::cerr << "[T159] close overtook in-flight send before enqueue\n";
        return 1;
    }
    if (!runCloseWaitsForInFlightSendAfterEnqueue()) {
        std::cerr << "[T159] close overtook in-flight send after enqueue\n";
        return 1;
    }
    if (!runCompletedSendBetweenEmptyCheckAndProducerScan()) {
        std::cerr << "[T159] close skipped second dequeue after producer scan\n";
        return 1;
    }
    if (!runCloseScanAcrossRepeatedSendCycle()) {
        std::cerr << "[T159] close reused stale idle state across send cycles\n";
        return 1;
    }

    std::cout << "T159-MpmcTimeoutRace PASS\n";
    return 0;
}
