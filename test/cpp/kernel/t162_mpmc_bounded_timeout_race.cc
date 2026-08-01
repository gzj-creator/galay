/**
 * @file t162_mpmc_bounded_timeout_race.cc
 * @brief 验证 MPMC bounded send/recv timeout 与对端操作只产生一个完成者。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/bounded_channel.h>
#include <galay/cpp/galay-kernel/core/scheduler.hpp>
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
    static bool enqueueRecvWaiter(
        BoundedChannel<T>& channel,
        const std::shared_ptr<BoundedChannelWaiter<T>>& waiter)
    {
        return channel.enqueueWaiter(channel.m_recvWaiters, waiter);
    }

    template <BoundedValue T>
    static bool enqueueValue(BoundedChannel<T>& channel, T&& value)
    {
        return channel.ringEnqueue(std::move(value));
    }

    template <BoundedValue T>
    static void requestRecvPump(BoundedChannel<T>& channel)
    {
        channel.requestPump(channel.kRecvWork);
    }

    template <BoundedValue T>
    static bool pumpIdle(const BoundedChannel<T>& channel)
    {
        return channel.m_pumpState.load(std::memory_order_acquire) == 0;
    }

    template <BoundedValue T>
    static void seedEmptyPosition(BoundedChannel<T>& channel,
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
    static uint64_t tailPosition(const BoundedChannel<T>& channel)
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

class ImmediateRaceScheduler final : public Scheduler
{
public:
    ImmediateRaceScheduler(galay::mpmc::BoundedChannel<int>* channel,
                           ImmediateAction action) noexcept
        : m_channel(channel), m_action(action)
    {
    }

    std::expected<void, IOError> start() override { return {}; }
    void stop() override {}

    bool schedule(TaskRef task) override
    {
        if (!bindTask(task)) {
            return false;
        }
        m_scheduleCalls.fetch_add(1, std::memory_order_relaxed);
        resume(task);
        return true;
    }

    bool scheduleDeferred(TaskRef task) override
    {
        return schedule(std::move(task));
    }

    bool scheduleImmediately(TaskRef task) override
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
        if (m_action == ImmediateAction::kPublishValue) {
            m_actionSucceeded.store(m_channel->trySend(73), std::memory_order_release);
        } else if (m_action == ImmediateAction::kReleaseSlot) {
            auto value = m_channel->tryRecv();
            m_actionSucceeded.store(
                value.has_value() && *value == 1,
                std::memory_order_release);
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

    bool actionSucceeded() const noexcept
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

class QueuedRaceScheduler final : public Scheduler
{
public:
    std::expected<void, IOError> start() override { return {}; }
    void stop() override {}

    bool schedule(TaskRef task) override
    {
        if (!bindTask(task)) {
            return false;
        }
        m_ready.push_back(std::move(task));
        return true;
    }

    bool scheduleDeferred(TaskRef task) override
    {
        return schedule(std::move(task));
    }

    bool scheduleImmediately(TaskRef task) override
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
Task<void> receiveWithTimeout(galay::mpmc::BoundedChannel<int>* channel,
                              OperationState* state)
{
    if constexpr (Batch) {
        auto result = co_await channel->recvBatch(4).timeout(1h);
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

Task<void> sendWithTimeout(galay::mpmc::BoundedChannel<int>* channel,
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

Task<void> receiveBlockingValue(
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

bool completedOnce(const OperationState& state, bool expectSuccess)
{
    return state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.succeeded.load(std::memory_order_acquire) == expectSuccess &&
        !state.closed.load(std::memory_order_acquire) &&
        state.timedOut.load(std::memory_order_acquire) != expectSuccess;
}

bool completedClosedOnce(const OperationState& state)
{
    return state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        !state.succeeded.load(std::memory_order_acquire) &&
        state.closed.load(std::memory_order_acquire) &&
        !state.timedOut.load(std::memory_order_acquire);
}

template <bool Batch>
bool runRecvImmediate(bool publishValue)
{
    galay::mpmc::BoundedChannel<int> channel(2);
    ImmediateRaceScheduler scheduler(
        &channel,
        publishValue ? ImmediateAction::kPublishValue : ImmediateAction::kNone);
    OperationState state;
    auto task = receiveWithTimeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    return started && completedOnce(state, publishValue) &&
        scheduler.addTimerCalls() == 1 && scheduler.scheduleCalls() == 0 &&
        (!publishValue || scheduler.actionSucceeded()) &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool runRecvProducerFirst()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = receiveWithTimeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool sent = channel.trySend(73);
    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool noDuplicate = scheduler.resumeWithTimerInDequeueGap();
    const bool passed = completedOnce(state, true);
    scheduler.releaseRetainedState();
    return started && sent && oneWake && noDuplicate && passed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool runRecvTimerFirst()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = receiveWithTimeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool timerFired = scheduler.fireTimer();
    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool sent = channel.trySend(73);
    const bool stillOneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    auto retained = channel.tryRecv();
    const bool passed = completedOnce(state, false);
    scheduler.releaseRetainedState();
    return started && timerFired && oneWake && sent && stillOneWake && resumed &&
        passed && retained.has_value() && *retained == 73 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool fillChannel(galay::mpmc::BoundedChannel<int>& channel)
{
    return channel.trySend(1) && channel.trySend(2) && channel.full();
}

bool runSendImmediate(bool releaseSlot)
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fillChannel(channel)) {
        return false;
    }
    ImmediateRaceScheduler scheduler(
        &channel,
        releaseSlot ? ImmediateAction::kReleaseSlot : ImmediateAction::kNone);
    OperationState state;
    auto task = sendWithTimeout(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    auto first = channel.tryRecv();
    auto second = channel.tryRecv();
    const bool queueCorrect = releaseSlot
        ? first.has_value() && second.has_value() && *first == 2 && *second == 73
        : first.has_value() && second.has_value() && *first == 1 && *second == 2;
    return started && completedOnce(state, releaseSlot) && queueCorrect &&
        scheduler.addTimerCalls() == 1 && scheduler.scheduleCalls() == 0 &&
        (!releaseSlot || scheduler.actionSucceeded()) &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool runSendConsumerFirst()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fillChannel(channel)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = sendWithTimeout(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    auto released = channel.tryRecv();
    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool noDuplicate = scheduler.resumeWithTimerInDequeueGap();
    auto first = channel.tryRecv();
    auto second = channel.tryRecv();
    const bool queueCorrect = released.has_value() && *released == 1 &&
        first.has_value() && *first == 2 && second.has_value() && *second == 73;
    const bool passed = completedOnce(state, true);
    scheduler.releaseRetainedState();
    return started && oneWake && noDuplicate && queueCorrect && passed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool runSendTimerFirst()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fillChannel(channel)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = sendWithTimeout(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool timerFired = scheduler.fireTimer();
    const bool oneWake = scheduler.hasSingleReadyTask();
    auto released = channel.tryRecv();
    const bool stillOneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    auto remaining = channel.tryRecv();
    auto unexpected = channel.tryRecv();
    const bool queueCorrect = released.has_value() && *released == 1 &&
        remaining.has_value() && *remaining == 2 && !unexpected.has_value();
    const bool passed = completedOnce(state, false);
    scheduler.releaseRetainedState();
    return started && timerFired && oneWake && stillOneWake && resumed &&
        queueCorrect && passed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

template <bool Batch>
bool runRecvCloseFirst()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = receiveWithTimeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    channel.close();
    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool noDuplicate = scheduler.resumeWithTimerInDequeueGap();
    const bool passed = completedClosedOnce(state);
    scheduler.releaseRetainedState();
    return started && oneWake && noDuplicate && passed && channel.empty() &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

template <bool Batch>
bool runRecvTimerThenClose()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = receiveWithTimeout<Batch>(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool timerFired = scheduler.fireTimer();
    const bool oneWake = scheduler.hasSingleReadyTask();
    channel.close();
    const bool stillOneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    const bool passed = completedOnce(state, false);
    scheduler.releaseRetainedState();
    return started && timerFired && oneWake && stillOneWake && resumed && passed &&
        channel.empty() && taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runSendCloseFirst()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fillChannel(channel)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = sendWithTimeout(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    channel.close();
    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool noDuplicate = scheduler.resumeWithTimerInDequeueGap();
    auto first = channel.tryRecv();
    auto second = channel.tryRecv();
    const bool queueCorrect = first.has_value() && second.has_value() &&
        *first == 1 && *second == 2 && !channel.tryRecv().has_value();
    const bool passed = completedClosedOnce(state);
    scheduler.releaseRetainedState();
    return started && oneWake && noDuplicate && queueCorrect && passed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runSendTimerThenClose()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fillChannel(channel)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    auto task = sendWithTimeout(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool timerFired = scheduler.fireTimer();
    const bool oneWake = scheduler.hasSingleReadyTask();
    channel.close();
    const bool stillOneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    auto first = channel.tryRecv();
    auto second = channel.tryRecv();
    const bool queueCorrect = first.has_value() && second.has_value() &&
        *first == 1 && *second == 2 && !channel.tryRecv().has_value();
    const bool passed = completedOnce(state, false);
    scheduler.releaseRetainedState();
    return started && timerFired && oneWake && stillOneWake && resumed &&
        queueCorrect && passed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

template <bool Batch>
bool runTimedOutRecvDoesNotSwallowMessage()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    QueuedRaceScheduler scheduler;
    OperationState timedOutState;
    OperationState liveState;
    auto timedOutTask = receiveWithTimeout<Batch>(&channel, &timedOutState);
    auto liveTask = receiveWithTimeout<Batch>(&channel, &liveState);
    TaskRef timedOutKeeper = detail::TaskAccess::taskRef(timedOutTask);
    TaskRef liveKeeper = detail::TaskAccess::taskRef(liveTask);
    TaskState* timedOutTaskState = timedOutKeeper.state();
    TaskState* liveTaskState = liveKeeper.state();
    if (timedOutTaskState == nullptr || liveTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled = detail::TaskAccess::detachTask(std::move(timedOutTask));
    TaskRef secondScheduled = detail::TaskAccess::detachTask(std::move(liveTask));
    const bool firstStarted = scheduler.scheduleImmediately(std::move(firstScheduled));
    const bool secondStarted = scheduler.scheduleImmediately(std::move(secondScheduled));
    const bool timerFired = scheduler.fireTimer(0);
    const bool timeoutReady = scheduler.readyCount() == 1;
    const bool sent = channel.trySend(73);
    const bool bothReady = scheduler.readyCount() == 2;
    const bool firstResumed = scheduler.runOne();
    const bool secondResumed = scheduler.runOne();
    const bool passed = completedOnce(timedOutState, false) &&
        completedOnce(liveState, true);
    scheduler.releaseRetainedState();
    return firstStarted && secondStarted && timerFired && timeoutReady && sent &&
        bothReady && firstResumed && secondResumed && passed && channel.empty() &&
        timedOutTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        liveTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runTimedOutSendDoesNotSwallowSlot()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    if (!fillChannel(channel)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState timedOutState;
    OperationState liveState;
    auto timedOutTask = sendWithTimeout(&channel, &timedOutState, 73);
    auto liveTask = sendWithTimeout(&channel, &liveState, 74);
    TaskRef timedOutKeeper = detail::TaskAccess::taskRef(timedOutTask);
    TaskRef liveKeeper = detail::TaskAccess::taskRef(liveTask);
    TaskState* timedOutTaskState = timedOutKeeper.state();
    TaskState* liveTaskState = liveKeeper.state();
    if (timedOutTaskState == nullptr || liveTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled = detail::TaskAccess::detachTask(std::move(timedOutTask));
    TaskRef secondScheduled = detail::TaskAccess::detachTask(std::move(liveTask));
    const bool firstStarted = scheduler.scheduleImmediately(std::move(firstScheduled));
    const bool secondStarted = scheduler.scheduleImmediately(std::move(secondScheduled));
    const bool timerFired = scheduler.fireTimer(0);
    const bool timeoutReady = scheduler.readyCount() == 1;
    auto released = channel.tryRecv();
    const bool bothReady = scheduler.readyCount() == 2;
    const bool firstResumed = scheduler.runOne();
    const bool secondResumed = scheduler.runOne();
    auto first = channel.tryRecv();
    auto second = channel.tryRecv();
    const bool queueCorrect = released.has_value() && *released == 1 &&
        first.has_value() && *first == 2 && second.has_value() && *second == 74 &&
        !channel.tryRecv().has_value();
    const bool passed = completedOnce(timedOutState, false) &&
        completedOnce(liveState, true);
    scheduler.releaseRetainedState();
    return firstStarted && secondStarted && timerFired && timeoutReady && bothReady &&
        firstResumed && secondResumed && queueCorrect && passed &&
        timedOutTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        liveTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runRecvPumpSkipsCancelledWaiter()
{
    galay::mpmc::BoundedChannel<int> channel(2);
    auto cancelled = std::make_shared<galay::mpmc::BoundedChannelWaiter<int>>(
        galay::kernel::Waker());
    auto live = std::make_shared<galay::mpmc::BoundedChannelWaiter<int>>(
        galay::kernel::Waker());
    if (!galay::mpmc::BoundedChannelTestAccess::enqueueRecvWaiter(channel, cancelled) ||
        !galay::mpmc::BoundedChannelTestAccess::enqueueRecvWaiter(channel, live)) {
        return false;
    }
    cancelled->state.store(galay::mpmc::BoundedWaiterState::kCancelled,
                           std::memory_order_release);
    if (!galay::mpmc::BoundedChannelTestAccess::enqueueValue(channel, 73)) {
        return false;
    }

    galay::mpmc::BoundedChannelTestAccess::requestRecvPump(channel);
    return live->state.load(std::memory_order_acquire) ==
            galay::mpmc::BoundedWaiterState::kFulfilled &&
        live->value.has_value() && *live->value == 73 && channel.empty() &&
        galay::mpmc::BoundedChannelTestAccess::pumpIdle(channel);
}

bool runCloseWaitsForInFlightSend()
{
    galay::mpmc::BoundedChannel<BlockingValue> channel(2);
    QueuedRaceScheduler scheduler;
    BlockingReceiveState receiveState;
    std::atomic<bool> moveEntered{false};
    std::atomic<bool> releaseMove{false};
    std::atomic<bool> sendSucceeded{false};

    std::thread producer([&] {
        BlockingValue value(73, &moveEntered, &releaseMove);
        sendSucceeded.store(channel.trySend(std::move(value)),
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
    auto task = receiveBlockingValue(&channel, &receiveState);
    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool stillWaiting = !receiveState.completed.load(std::memory_order_acquire);

    releaseMove.store(true, std::memory_order_release);
    producer.join();
    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    scheduler.releaseRetainedState();
    return started && stillWaiting && sendSucceeded.load(std::memory_order_acquire) &&
        oneWake && resumed && receiveState.completed.load(std::memory_order_acquire) &&
        receiveState.succeeded.load(std::memory_order_acquire) &&
        !receiveState.closed.load(std::memory_order_acquire) && channel.empty();
}

bool runTailCloseBitBoundary()
{
    constexpr uint64_t kPositionMask = (uint64_t{1} << 63U) - 1U;

    galay::mpmc::BoundedChannel<int> lastReservation(2);
    galay::mpmc::BoundedChannelTestAccess::seedEmptyPosition(
        lastReservation, kPositionMask - 1U);
    int value = 91;
    if (!lastReservation.trySend(std::move(value)) ||
        lastReservation.isClosed() ||
        galay::mpmc::BoundedChannelTestAccess::tailPosition(lastReservation) !=
            kPositionMask) {
        return false;
    }
    lastReservation.close();
    auto received = lastReservation.tryRecv();
    if (!lastReservation.isClosed() || !received.has_value() || *received != 91 ||
        !lastReservation.empty()) {
        return false;
    }

    galay::mpmc::BoundedChannel<int> exhausted(2);
    galay::mpmc::BoundedChannelTestAccess::seedEmptyPosition(exhausted,
                                                            kPositionMask);
    int rejected = 92;
    return !exhausted.trySend(std::move(rejected)) && exhausted.isClosed() &&
        galay::mpmc::BoundedChannelTestAccess::tailPosition(exhausted) ==
            kPositionMask;
}

}  // namespace

int main()
{
    if (!runRecvImmediate<false>(true) || !runRecvImmediate<false>(false) ||
        !runRecvProducerFirst<false>() || !runRecvTimerFirst<false>() ||
        !runRecvCloseFirst<false>() || !runRecvTimerThenClose<false>()) {
        std::cerr << "[T162] MPMC bounded recv timeout arbitration failed\n";
        return 1;
    }
    if (!runRecvImmediate<true>(true) || !runRecvImmediate<true>(false) ||
        !runRecvProducerFirst<true>() || !runRecvTimerFirst<true>() ||
        !runRecvCloseFirst<true>() || !runRecvTimerThenClose<true>()) {
        std::cerr << "[T162] MPMC bounded recvBatch timeout arbitration failed\n";
        return 1;
    }
    if (!runSendImmediate(true) || !runSendImmediate(false) ||
        !runSendConsumerFirst() || !runSendTimerFirst() ||
        !runSendCloseFirst() || !runSendTimerThenClose()) {
        std::cerr << "[T162] MPMC bounded send timeout arbitration failed\n";
        return 1;
    }
    if (!runTimedOutRecvDoesNotSwallowMessage<false>() ||
        !runTimedOutRecvDoesNotSwallowMessage<true>() ||
        !runTimedOutSendDoesNotSwallowSlot()) {
        std::cerr << "[T162] timed-out waiter swallowed a resource event\n";
        return 1;
    }
    if (!runRecvPumpSkipsCancelledWaiter()) {
        std::cerr << "[T162] recv pump stopped at a cancelled waiter\n";
        return 1;
    }
    if (!runCloseWaitsForInFlightSend()) {
        std::cerr << "[T162] close overtook an in-flight send publication\n";
        return 1;
    }
    if (!runTailCloseBitBoundary()) {
        std::cerr << "[T162] tail close-bit boundary protocol failed\n";
        return 1;
    }

    std::cout << "T162-MpmcBoundedTimeoutRace PASS\n";
    return 0;
}
