/**
 * @file t161_mpsc_bounded_timeout_race.cc
 * @brief 验证 MPSC bounded send/recv/recvBatch 与 timeout 只产生一个完成者。
 */

#define GALAY_MPSC_BOUNDED_TEST_HOOKS 1
#include <galay/cpp/galay-kernel/concurrency/mpsc/bounded_channel.h>
#include <galay/cpp/galay-kernel/core/scheduler.hpp>
#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace galay::kernel;
using namespace std::chrono_literals;

using Channel = galay::mpsc::BoundedChannel<int>;

enum class OperationKind {
    kSend,
    kRecv,
    kRecvBatch,
};

constexpr OperationKind kOperationKinds[]{
    OperationKind::kSend,
    OperationKind::kRecv,
    OperationKind::kRecvBatch,
};

const char* operationName(OperationKind kind) noexcept
{
    switch (kind) {
    case OperationKind::kSend:
        return "send";
    case OperationKind::kRecv:
        return "recv";
    case OperationKind::kRecvBatch:
        return "recvBatch";
    }
    return "unknown";
}

struct OperationState
{
    std::atomic<size_t> valueCount{0};
    std::atomic<int> valueSum{0};
    std::atomic<int> resumeCount{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> succeeded{false};
    std::atomic<bool> timedOut{false};
    std::atomic<bool> closed{false};
};

struct BlockingMoveControl
{
    std::atomic<bool> moveEntered{false};
    std::atomic<bool> releaseMove{false};
};

struct BlockingValue
{
    int value = 0;
    BlockingMoveControl* control = nullptr;
    bool blockOnMove = false;

    BlockingValue() = default;

    BlockingValue(int inputValue,
                  BlockingMoveControl* inputControl,
                  bool shouldBlock) noexcept
        : value(inputValue), control(inputControl), blockOnMove(shouldBlock)
    {
    }

    BlockingValue(const BlockingValue&) = delete;
    BlockingValue& operator=(const BlockingValue&) = delete;

    BlockingValue(BlockingValue&& other) noexcept
        : value(other.value), control(other.control)
    {
        const bool shouldBlock = std::exchange(other.blockOnMove, false);
        if (shouldBlock && control != nullptr) {
            control->moveEntered.store(true, std::memory_order_release);
            control->moveEntered.notify_all();
            control->releaseMove.wait(false, std::memory_order_acquire);
        }
    }

    BlockingValue& operator=(BlockingValue&& other) noexcept
    {
        value = other.value;
        control = other.control;
        blockOnMove = false;
        other.blockOnMove = false;
        return *this;
    }
};

struct BlockingRecvState
{
    std::atomic<int> resumeCount{0};
    std::atomic<bool> completed{false};
    bool succeeded = false;
    bool closed = false;
    int value = 0;
};

struct PumpExitRaceControl
{
    std::atomic<bool> ownerReached{false};
    std::atomic<bool> releaseOwner{false};

    static void hook(galay::mpsc::bounded_detail::TestHookPoint point,
                     void* context) noexcept
    {
        if (point !=
            galay::mpsc::bounded_detail::TestHookPoint::kPumpBeforeRelease) {
            return;
        }
        auto& control = *static_cast<PumpExitRaceControl*>(context);
        control.ownerReached.store(true, std::memory_order_release);
        control.ownerReached.notify_all();
        control.releaseOwner.wait(false, std::memory_order_acquire);
    }
};

struct PreArmRaceControl
{
    OperationKind kind;
    std::atomic<bool> ownerReached{false};
    std::atomic<bool> releaseOwner{false};

    static void hook(galay::mpsc::bounded_detail::TestHookPoint point,
                     void* context) noexcept
    {
        auto& control = *static_cast<PreArmRaceControl*>(context);
        const bool expectedPoint = control.kind == OperationKind::kSend
            ? point == galay::mpsc::bounded_detail::TestHookPoint::kSendBeforeArm
            : point == galay::mpsc::bounded_detail::TestHookPoint::kRecvBeforeArm;
        if (!expectedPoint) {
            return;
        }
        control.ownerReached.store(true, std::memory_order_release);
        control.ownerReached.notify_all();
        control.releaseOwner.wait(false, std::memory_order_acquire);
    }
};

bool waitForFlag(const std::atomic<bool>& flag) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::yield();
    }
    return flag.load(std::memory_order_acquire);
}

bool prepareChannel(Channel& channel, OperationKind kind)
{
    if (kind != OperationKind::kSend) {
        return true;
    }
    return channel.trySend(10) && channel.trySend(11) && channel.full();
}

bool triggerOperation(Channel& channel, OperationKind kind)
{
    if (kind == OperationKind::kSend) {
        auto released = channel.tryRecv();
        return released.has_value() && *released == 10;
    }
    return channel.trySend(73);
}

bool drainExact(Channel& channel, int first, int second = -1)
{
    auto firstValue = channel.tryRecv();
    if (!firstValue.has_value() || *firstValue != first) {
        return false;
    }
    if (second >= 0) {
        auto secondValue = channel.tryRecv();
        if (!secondValue.has_value() || *secondValue != second) {
            return false;
        }
    }
    return channel.empty();
}

bool operationResultMatches(const OperationState& state,
                            OperationKind kind) noexcept
{
    if (!state.succeeded.load(std::memory_order_acquire) ||
        state.timedOut.load(std::memory_order_acquire) ||
        state.closed.load(std::memory_order_acquire)) {
        return false;
    }
    if (kind == OperationKind::kSend) {
        return state.valueCount.load(std::memory_order_acquire) == 0;
    }
    return state.valueCount.load(std::memory_order_acquire) == 1 &&
        state.valueSum.load(std::memory_order_acquire) == 73;
}

bool timeoutResultMatches(const OperationState& state) noexcept
{
    return !state.succeeded.load(std::memory_order_acquire) &&
        state.timedOut.load(std::memory_order_acquire) &&
        !state.closed.load(std::memory_order_acquire) &&
        state.valueCount.load(std::memory_order_acquire) == 0;
}

bool closeResultMatches(const OperationState& state) noexcept
{
    return !state.succeeded.load(std::memory_order_acquire) &&
        !state.timedOut.load(std::memory_order_acquire) &&
        state.closed.load(std::memory_order_acquire) &&
        state.valueCount.load(std::memory_order_acquire) == 0;
}

bool channelMatchesOperationResult(Channel& channel, OperationKind kind)
{
    if (kind == OperationKind::kSend) {
        return drainExact(channel, 11, 73);
    }
    return channel.empty();
}

bool channelMatchesUntouchedTimeout(Channel& channel, OperationKind kind)
{
    if (kind == OperationKind::kSend) {
        return drainExact(channel, 10, 11);
    }
    return channel.empty();
}

bool channelMatchesTimerFirstResult(Channel& channel, OperationKind kind)
{
    if (kind == OperationKind::kSend) {
        // triggerOperation() 只释放 10；超时发送的 73 不得进入 ring。
        return drainExact(channel, 11);
    }
    // timer 已获胜时，producer 发布的消息必须留给下一次接收。
    return drainExact(channel, 73);
}

class ImmediateRaceScheduler final : public Scheduler
{
public:
    ImmediateRaceScheduler(Channel* channel,
                           OperationKind kind,
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
        if (m_completeOperation) {
            m_actionSucceeded.store(
                triggerOperation(*m_channel, m_kind), std::memory_order_release);
        }
        // 强制 WithTimeout 在 inner waiter 发布后同步执行 timeoutNow()。
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
    Channel* m_channel;
    const OperationKind m_kind;
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

    bool fireTimer(size_t index = 0)
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

        // 精确覆盖 resumeTaskState() 已出队、尚未 handle.resume() 的窗口。
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

Task<void> runWithTimeout(Channel* channel,
                          OperationKind kind,
                          OperationState* state)
{
    if (kind == OperationKind::kSend) {
        auto result = co_await channel->send(73).timeout(1h);
        state->succeeded.store(result.has_value(), std::memory_order_release);
        if (!result.has_value()) {
            state->timedOut.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
            state->closed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
        }
    } else if (kind == OperationKind::kRecv) {
        auto result = co_await channel->recv().timeout(1h);
        state->succeeded.store(result.has_value(), std::memory_order_release);
        if (result.has_value()) {
            state->valueCount.store(1, std::memory_order_release);
            state->valueSum.store(*result, std::memory_order_release);
        } else {
            state->timedOut.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
            state->closed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
        }
    } else {
        auto result = co_await channel->recvBatch(2).timeout(1h);
        state->succeeded.store(result.has_value(), std::memory_order_release);
        if (result.has_value()) {
            int sum = 0;
            for (int value : *result) {
                sum += value;
            }
            state->valueCount.store(result->size(), std::memory_order_release);
            state->valueSum.store(sum, std::memory_order_release);
        } else {
            state->timedOut.store(
                IOError::contains(result.error().code(), kTimeout),
                std::memory_order_release);
            state->closed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
        }
    }
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    state->completed.store(true, std::memory_order_release);
    co_return;
}

Task<void> runWithoutTimeout(Channel* channel,
                             OperationKind kind,
                             OperationState* state)
{
    if (kind == OperationKind::kSend) {
        auto result = co_await channel->send(73);
        state->succeeded.store(result.has_value(), std::memory_order_release);
        if (!result.has_value()) {
            state->closed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
        }
    } else if (kind == OperationKind::kRecv) {
        auto result = co_await channel->recv();
        state->succeeded.store(result.has_value(), std::memory_order_release);
        if (result.has_value()) {
            state->valueCount.store(1, std::memory_order_release);
            state->valueSum.store(*result, std::memory_order_release);
        } else {
            state->closed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
        }
    } else {
        auto result = co_await channel->recvBatch(2);
        state->succeeded.store(result.has_value(), std::memory_order_release);
        if (result.has_value()) {
            int sum = 0;
            for (int value : *result) {
                sum += value;
            }
            state->valueCount.store(result->size(), std::memory_order_release);
            state->valueSum.store(sum, std::memory_order_release);
        } else {
            state->closed.store(
                IOError::contains(result.error().code(), kClosed),
                std::memory_order_release);
        }
    }
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    state->completed.store(true, std::memory_order_release);
    co_return;
}

bool runCompletionBeforeAwaiterArm(OperationKind kind)
{
    Channel channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    PreArmRaceControl control{kind};
    galay::mpsc::bounded_detail::setTestHook(
        &PreArmRaceControl::hook, &control);

    auto task = runWithoutTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        galay::mpsc::bounded_detail::clearTestHook();
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    bool started = false;
    std::thread owner(
        [&scheduler, &started, scheduled = std::move(scheduled)]() mutable {
            started = scheduler.scheduleImmediately(std::move(scheduled));
        });

    const bool ownerBlocked = waitForFlag(control.ownerReached);
    const bool triggered = ownerBlocked && triggerOperation(channel, kind);
    const bool noEarlySchedule = scheduler.readyCount() == 0;

    control.releaseOwner.store(true, std::memory_order_release);
    control.releaseOwner.notify_all();
    owner.join();
    galay::mpsc::bounded_detail::clearTestHook();

    bool resumed = state.completed.load(std::memory_order_acquire);
    if (!resumed && scheduler.hasSingleReadyTask()) {
        resumed = scheduler.runOne();
    }
    const bool completed = resumed &&
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        operationResultMatches(state, kind);
    const bool channelMatches = completed &&
        channelMatchesOperationResult(channel, kind);

    scheduler.releaseRetainedState();
    return ownerBlocked && started && triggered && noEarlySchedule &&
        completed && channelMatches &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

Task<void> recvBlockingValue(
    galay::mpsc::BoundedChannel<BlockingValue>* channel,
    BlockingRecvState* state)
{
    auto result = co_await channel->recv();
    state->succeeded = result.has_value();
    if (result.has_value()) {
        state->value = result->value;
    } else {
        state->closed = IOError::contains(result.error().code(), kClosed);
    }
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    state->completed.store(true, std::memory_order_release);
    co_return;
}

bool runCloseWaitsForReservedProducer()
{
    using BlockingChannel = galay::mpsc::BoundedChannel<BlockingValue>;

    BlockingChannel channel(2);
    BlockingMoveControl control;
    BlockingValue value(91, &control, true);
    std::atomic<bool> sendSucceeded{false};
    std::thread producer([&] {
        sendSucceeded.store(channel.trySend(std::move(value)),
                            std::memory_order_release);
    });

    const bool producerReserved = waitForFlag(control.moveEntered);
    if (!producerReserved) {
        control.releaseMove.store(true, std::memory_order_release);
        control.releaseMove.notify_all();
        producer.join();
        return false;
    }

    channel.close();
    QueuedRaceScheduler scheduler;
    BlockingRecvState state;
    auto task = recvBlockingValue(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        control.releaseMove.store(true, std::memory_order_release);
        control.releaseMove.notify_all();
        producer.join();
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool stayedPending =
        !state.completed.load(std::memory_order_acquire) &&
        scheduler.readyCount() == 0;

    control.releaseMove.store(true, std::memory_order_release);
    control.releaseMove.notify_all();
    producer.join();

    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool resumed = oneWake && scheduler.runOne();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.succeeded && !state.closed && state.value == 91;

    scheduler.releaseRetainedState();
    const bool passed = started && stayedPending &&
        sendSucceeded.load(std::memory_order_acquire) && oneWake && resumed &&
        completed && channel.empty() && channel.isClosed() &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
    if (!passed) {
        std::cerr << "reserved_close details: started=" << started
                  << " stayed_pending=" << stayedPending
                  << " send=" << sendSucceeded.load(std::memory_order_relaxed)
                  << " one_wake=" << oneWake << " resumed=" << resumed
                  << " completed=" << completed << " success=" << state.succeeded
                  << " closed=" << state.closed << " value=" << state.value
                  << " empty=" << channel.empty()
                  << " refs=" << taskState->m_refs.load(std::memory_order_relaxed)
                  << '\n';
    }
    return passed;
}

bool runImmediateCompletion(OperationKind kind, bool completeOperation)
{
    Channel channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    ImmediateRaceScheduler scheduler(&channel, kind, completeOperation);
    OperationState state;

    auto task = runWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool resultMatches = completeOperation
        ? operationResultMatches(state, kind) && scheduler.actionSucceeded()
        : timeoutResultMatches(state);
    const bool channelMatches = completeOperation
        ? channelMatchesOperationResult(channel, kind)
        : channelMatchesUntouchedTimeout(channel, kind);

    return started && state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 && resultMatches &&
        channelMatches && scheduler.addTimerCalls() == 1 &&
        scheduler.scheduleCalls() == 0 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runOperationTimerArbitration(OperationKind kind)
{
    Channel channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;

    auto task = runWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool triggered = triggerOperation(channel, kind);
    const bool oneOperationWake = scheduler.hasSingleReadyTask();
    const bool noDuplicateWake = scheduler.resumeWithTimerInDequeueGap();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        operationResultMatches(state, kind);
    const bool channelMatches = channelMatchesOperationResult(channel, kind);

    scheduler.releaseRetainedState();
    return started && triggered && oneOperationWake && noDuplicateWake &&
        completed && channelMatches &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runTimerOperationArbitration(OperationKind kind)
{
    Channel channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;

    auto task = runWithTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool timerFired = scheduler.fireTimer();
    const bool oneTimerWake = scheduler.hasSingleReadyTask();
    const bool triggered = triggerOperation(channel, kind);
    const bool stillOneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    const bool timedOut = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        timeoutResultMatches(state);
    const bool channelMatches = channelMatchesTimerFirstResult(channel, kind);

    scheduler.releaseRetainedState();
    return started && timerFired && oneTimerWake && triggered && stillOneWake &&
        resumed && timedOut && channelMatches &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runCloseTimerArbitration(OperationKind kind, bool closeFirst)
{
    Channel channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;

    auto task = runWithTimeout(&channel, kind, &state);
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
        (closeFirst ? closeResultMatches(state) : timeoutResultMatches(state));
    const bool channelMatches = channelMatchesUntouchedTimeout(channel, kind);

    scheduler.releaseRetainedState();
    return started && oneWake && noDuplicateWake && resumed && completed &&
        channelMatches &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runPumpExitWorkRace(OperationKind kind)
{
    // owner 已处理完当前快照、尚未清除 Running 时到达的新 work，必须让退出
    // CAS 失败并继续一轮，不能把最后一次 message/slot 事件遗留在队列中。
    Channel channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    PumpExitRaceControl control;
    galay::mpsc::bounded_detail::setTestHook(
        &PumpExitRaceControl::hook, &control);

    auto task = runWithoutTimeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        galay::mpsc::bounded_detail::clearTestHook();
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    bool started = false;
    std::thread owner([&scheduler, &started, scheduled = std::move(scheduled)]() mutable {
        started = scheduler.scheduleImmediately(std::move(scheduled));
    });

    const bool ownerBlocked = waitForFlag(control.ownerReached);
    const bool triggered = ownerBlocked && triggerOperation(channel, kind);
    const bool noEarlyWake = scheduler.readyCount() == 0;
    control.releaseOwner.store(true, std::memory_order_release);
    control.releaseOwner.notify_all();
    owner.join();
    galay::mpsc::bounded_detail::clearTestHook();

    bool resumed = state.completed.load(std::memory_order_acquire);
    if (!resumed && scheduler.hasSingleReadyTask()) {
        resumed = scheduler.runOne();
    }
    const bool completed = resumed &&
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        operationResultMatches(state, kind);
    const bool channelMatches = completed &&
        channelMatchesOperationResult(channel, kind);

    scheduler.releaseRetainedState();
    return ownerBlocked && started && triggered && noEarlyWake && completed &&
        channelMatches &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool runTimedOutWaiterDoesNotSwallowResource(OperationKind kind)
{
    // timer 已裁决但任务尚未恢复时，队首 waiter 是 tombstone；scanner 必须
    // 越过它，把同一个资源事件交给后面的 live waiter。
    Channel channel(2);
    if (!prepareChannel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState timedOutState;
    OperationState liveState;
    auto timedOutTask = runWithTimeout(&channel, kind, &timedOutState);
    auto liveTask = runWithTimeout(&channel, kind, &liveState);
    TaskRef timedOutKeeper = detail::TaskAccess::taskRef(timedOutTask);
    TaskRef liveKeeper = detail::TaskAccess::taskRef(liveTask);
    TaskState* timedOutTaskState = timedOutKeeper.state();
    TaskState* liveTaskState = liveKeeper.state();
    if (timedOutTaskState == nullptr || liveTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled =
        detail::TaskAccess::detachTask(std::move(timedOutTask));
    TaskRef secondScheduled = detail::TaskAccess::detachTask(std::move(liveTask));
    const bool firstStarted =
        scheduler.scheduleImmediately(std::move(firstScheduled));
    const bool secondStarted =
        scheduler.scheduleImmediately(std::move(secondScheduled));
    const bool timerFired = scheduler.fireTimer(0);
    const bool timeoutReady = scheduler.readyCount() == 1;
    const bool triggered = triggerOperation(channel, kind);
    const bool bothReady = scheduler.readyCount() == 2;
    const bool firstResumed = scheduler.runOne();
    const bool secondResumed = bothReady && scheduler.runOne();
    const bool completed = timeoutResultMatches(timedOutState) &&
        timedOutState.resumeCount.load(std::memory_order_acquire) == 1 &&
        operationResultMatches(liveState, kind) &&
        liveState.resumeCount.load(std::memory_order_acquire) == 1;
    const bool channelMatches = completed &&
        channelMatchesOperationResult(channel, kind);

    if (!bothReady) {
        channel.close();
        while (scheduler.runOne()) {
        }
    }
    scheduler.releaseRetainedState();
    return firstStarted && secondStarted && timerFired && timeoutReady &&
        triggered && bothReady && firstResumed && secondResumed && completed &&
        channelMatches &&
        timedOutTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        liveTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

}  // namespace

int main()
{
    if (!runCloseWaitsForReservedProducer()) {
        std::cerr << "[T161] close overtook reserved producer publication\n";
        return 1;
    }

    for (OperationKind kind : kOperationKinds) {
        if (!runCompletionBeforeAwaiterArm(kind)) {
            std::cerr << "[T161] " << operationName(kind)
                      << " completion scheduled before awaiter arm\n";
            return 1;
        }
        if (!runImmediateCompletion(kind, true)) {
            std::cerr << "[T161] " << operationName(kind)
                      << " operation-win addTimer=false failed\n";
            return 1;
        }
        if (!runImmediateCompletion(kind, false)) {
            std::cerr << "[T161] " << operationName(kind)
                      << " timeout-win addTimer=false failed\n";
            return 1;
        }
        if (!runOperationTimerArbitration(kind)) {
            std::cerr << "[T161] " << operationName(kind)
                      << " operation-first dequeue-gap failed\n";
            return 1;
        }
        if (!runTimerOperationArbitration(kind)) {
            std::cerr << "[T161] " << operationName(kind)
                      << " timer-first preservation failed\n";
            return 1;
        }
        if (!runCloseTimerArbitration(kind, true)) {
            std::cerr << "[T161] " << operationName(kind)
                      << " close-first arbitration failed\n";
            return 1;
        }
        if (!runCloseTimerArbitration(kind, false)) {
            std::cerr << "[T161] " << operationName(kind)
                      << " timer-first close arbitration failed\n";
            return 1;
        }
        if (!runPumpExitWorkRace(kind)) {
            std::cerr << "[T161] " << operationName(kind)
                      << " pump exit lost late work\n";
            return 1;
        }
        if (!runTimedOutWaiterDoesNotSwallowResource(kind)) {
            std::cerr << "[T161] " << operationName(kind)
                      << " timed-out waiter swallowed resource\n";
            return 1;
        }
    }

    std::cout << "T161-MpscBoundedTimeoutRace PASS\n";
    return 0;
}
