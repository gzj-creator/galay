/**
 * @file t161_mpsc_bounded_timeout_race.cc
 * @brief 验证 MPSC bounded send/recv/recv_batch 与 timeout 只产生一个完成者。
 */

#define GALAY_MPSC_BOUNDED_TEST_HOOKS 1
#include <galay/cpp/galay-kernel/concurrency/mpsc/bounded_channel.h>
#include "test/cpp/common/scheduler_test_adapter.h"
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

const char* operation_name(OperationKind kind) noexcept
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

bool wait_for_flag(const std::atomic<bool>& flag) noexcept
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

bool prepare_channel(Channel& channel, OperationKind kind)
{
    if (kind != OperationKind::kSend) {
        return true;
    }
    return channel.try_send(10) && channel.try_send(11) && channel.full();
}

bool trigger_operation(Channel& channel, OperationKind kind)
{
    if (kind == OperationKind::kSend) {
        auto released = channel.try_recv();
        return released.has_value() && *released == 10;
    }
    return channel.try_send(73);
}

bool drain_exact(Channel& channel, int first, int second = -1)
{
    auto firstValue = channel.try_recv();
    if (!firstValue.has_value() || *firstValue != first) {
        return false;
    }
    if (second >= 0) {
        auto secondValue = channel.try_recv();
        if (!secondValue.has_value() || *secondValue != second) {
            return false;
        }
    }
    return channel.empty();
}

bool operation_result_matches(const OperationState& state,
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

bool timeout_result_matches(const OperationState& state) noexcept
{
    return !state.succeeded.load(std::memory_order_acquire) &&
        state.timedOut.load(std::memory_order_acquire) &&
        !state.closed.load(std::memory_order_acquire) &&
        state.valueCount.load(std::memory_order_acquire) == 0;
}

bool close_result_matches(const OperationState& state) noexcept
{
    return !state.succeeded.load(std::memory_order_acquire) &&
        !state.timedOut.load(std::memory_order_acquire) &&
        state.closed.load(std::memory_order_acquire) &&
        state.valueCount.load(std::memory_order_acquire) == 0;
}

bool channel_matches_operation_result(Channel& channel, OperationKind kind)
{
    if (kind == OperationKind::kSend) {
        return drain_exact(channel, 11, 73);
    }
    return channel.empty();
}

bool channel_matches_untouched_timeout(Channel& channel, OperationKind kind)
{
    if (kind == OperationKind::kSend) {
        return drain_exact(channel, 10, 11);
    }
    return channel.empty();
}

bool channel_matches_timer_first_result(Channel& channel, OperationKind kind)
{
    if (kind == OperationKind::kSend) {
        // trigger_operation() 只释放 10；超时发送的 73 不得进入 ring。
        return drain_exact(channel, 11);
    }
    // timer 已获胜时，producer 发布的消息必须留给下一次接收。
    return drain_exact(channel, 73);
}

class ImmediateRaceScheduler final : public detail::SchedulerTestAdapter<ImmediateRaceScheduler>
{
public:
    ImmediateRaceScheduler(Channel* channel,
                           OperationKind kind,
                           bool completeOperation) noexcept
        : m_channel(channel), m_kind(kind), m_completeOperation(completeOperation)
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
        if (m_completeOperation) {
            m_actionSucceeded.store(
                trigger_operation(*m_channel, m_kind), std::memory_order_release);
        }
        // 强制 WithTimeout 在 inner waiter 发布后同步执行 timeout_now()。
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
    Channel* m_channel;
    const OperationKind m_kind;
    const bool m_completeOperation;
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

    bool fire_timer(size_t index = 0)
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
            state->m_done.load(std::memory_order_acquire)) {
            return false;
        }

        // 精确覆盖 resume_task_state() 已出队、尚未 handle.resume() 的窗口。
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

Task<void> run_with_timeout(Channel* channel,
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
        auto result = co_await channel->recv_batch(2).timeout(1h);
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

Task<void> run_without_timeout(Channel* channel,
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
        auto result = co_await channel->recv_batch(2);
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

bool run_completion_before_awaiter_arm(OperationKind kind)
{
    Channel channel(2);
    if (!prepare_channel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    PreArmRaceControl control{kind};
    galay::mpsc::bounded_detail::set_test_hook(
        &PreArmRaceControl::hook, &control);

    auto task = run_without_timeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        galay::mpsc::bounded_detail::clear_test_hook();
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    bool started = false;
    std::thread owner(
        [&scheduler, &started, scheduled = std::move(scheduled)]() mutable {
            started = scheduler.schedule_immediately(std::move(scheduled));
        });

    const bool ownerBlocked = wait_for_flag(control.ownerReached);
    const bool triggered = ownerBlocked && trigger_operation(channel, kind);
    const bool noEarlySchedule = scheduler.ready_count() == 0;

    control.releaseOwner.store(true, std::memory_order_release);
    control.releaseOwner.notify_all();
    owner.join();
    galay::mpsc::bounded_detail::clear_test_hook();

    bool resumed = state.completed.load(std::memory_order_acquire);
    if (!resumed && scheduler.has_single_ready_task()) {
        resumed = scheduler.run_one();
    }
    const bool completed = resumed &&
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        operation_result_matches(state, kind);
    const bool channelMatches = completed &&
        channel_matches_operation_result(channel, kind);

    scheduler.release_retained_state();
    return ownerBlocked && started && triggered && noEarlySchedule &&
        completed && channelMatches &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

Task<void> recv_blocking_value(
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

bool run_close_waits_for_reserved_producer()
{
    using BlockingChannel = galay::mpsc::BoundedChannel<BlockingValue>;

    BlockingChannel channel(2);
    BlockingMoveControl control;
    BlockingValue value(91, &control, true);
    std::atomic<bool> sendSucceeded{false};
    std::thread producer([&] {
        sendSucceeded.store(channel.try_send(std::move(value)),
                            std::memory_order_release);
    });

    const bool producerReserved = wait_for_flag(control.moveEntered);
    if (!producerReserved) {
        control.releaseMove.store(true, std::memory_order_release);
        control.releaseMove.notify_all();
        producer.join();
        return false;
    }

    channel.close();
    QueuedRaceScheduler scheduler;
    BlockingRecvState state;
    auto task = recv_blocking_value(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        control.releaseMove.store(true, std::memory_order_release);
        control.releaseMove.notify_all();
        producer.join();
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool stayedPending =
        !state.completed.load(std::memory_order_acquire) &&
        scheduler.ready_count() == 0;

    control.releaseMove.store(true, std::memory_order_release);
    control.releaseMove.notify_all();
    producer.join();

    const bool oneWake = scheduler.has_single_ready_task();
    const bool resumed = oneWake && scheduler.run_one();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.succeeded && !state.closed && state.value == 91;

    scheduler.release_retained_state();
    const bool passed = started && stayedPending &&
        sendSucceeded.load(std::memory_order_acquire) && oneWake && resumed &&
        completed && channel.empty() && channel.is_closed() &&
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

bool run_immediate_completion(OperationKind kind, bool completeOperation)
{
    Channel channel(2);
    if (!prepare_channel(channel, kind)) {
        return false;
    }
    ImmediateRaceScheduler scheduler(&channel, kind, completeOperation);
    OperationState state;

    auto task = run_with_timeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool resultMatches = completeOperation
        ? operation_result_matches(state, kind) && scheduler.action_succeeded()
        : timeout_result_matches(state);
    const bool channelMatches = completeOperation
        ? channel_matches_operation_result(channel, kind)
        : channel_matches_untouched_timeout(channel, kind);

    return started && state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 && resultMatches &&
        channelMatches && scheduler.add_timer_calls() == 1 &&
        scheduler.schedule_calls() == 0 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_operation_timer_arbitration(OperationKind kind)
{
    Channel channel(2);
    if (!prepare_channel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;

    auto task = run_with_timeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool triggered = trigger_operation(channel, kind);
    const bool oneOperationWake = scheduler.has_single_ready_task();
    const bool noDuplicateWake = scheduler.resume_with_timer_in_dequeue_gap();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        operation_result_matches(state, kind);
    const bool channelMatches = channel_matches_operation_result(channel, kind);

    scheduler.release_retained_state();
    return started && triggered && oneOperationWake && noDuplicateWake &&
        completed && channelMatches &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_timer_operation_arbitration(OperationKind kind)
{
    Channel channel(2);
    if (!prepare_channel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;

    auto task = run_with_timeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool timerFired = scheduler.fire_timer();
    const bool oneTimerWake = scheduler.has_single_ready_task();
    const bool triggered = trigger_operation(channel, kind);
    const bool stillOneWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    const bool timedOut = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        timeout_result_matches(state);
    const bool channelMatches = channel_matches_timer_first_result(channel, kind);

    scheduler.release_retained_state();
    return started && timerFired && oneTimerWake && triggered && stillOneWake &&
        resumed && timedOut && channelMatches &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_close_timer_arbitration(OperationKind kind, bool closeFirst)
{
    Channel channel(2);
    if (!prepare_channel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;

    auto task = run_with_timeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    bool oneWake = false;
    bool noDuplicateWake = false;
    bool resumed = false;
    if (closeFirst) {
        channel.close();
        oneWake = scheduler.has_single_ready_task();
        noDuplicateWake = scheduler.resume_with_timer_in_dequeue_gap();
        resumed = true;
    } else {
        const bool timerFired = scheduler.fire_timer();
        oneWake = timerFired && scheduler.has_single_ready_task();
        channel.close();
        noDuplicateWake = scheduler.has_single_ready_task();
        resumed = scheduler.run_one();
    }

    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        (closeFirst ? close_result_matches(state) : timeout_result_matches(state));
    const bool channelMatches = channel_matches_untouched_timeout(channel, kind);

    scheduler.release_retained_state();
    return started && oneWake && noDuplicateWake && resumed && completed &&
        channelMatches &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_pump_exit_work_race(OperationKind kind)
{
    // owner 已处理完当前快照、尚未清除 Running 时到达的新 work，必须让退出
    // CAS 失败并继续一轮，不能把最后一次 message/slot 事件遗留在队列中。
    Channel channel(2);
    if (!prepare_channel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState state;
    PumpExitRaceControl control;
    galay::mpsc::bounded_detail::set_test_hook(
        &PumpExitRaceControl::hook, &control);

    auto task = run_without_timeout(&channel, kind, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        galay::mpsc::bounded_detail::clear_test_hook();
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    bool started = false;
    std::thread owner([&scheduler, &started, scheduled = std::move(scheduled)]() mutable {
        started = scheduler.schedule_immediately(std::move(scheduled));
    });

    const bool ownerBlocked = wait_for_flag(control.ownerReached);
    const bool triggered = ownerBlocked && trigger_operation(channel, kind);
    const bool noEarlyWake = scheduler.ready_count() == 0;
    control.releaseOwner.store(true, std::memory_order_release);
    control.releaseOwner.notify_all();
    owner.join();
    galay::mpsc::bounded_detail::clear_test_hook();

    bool resumed = state.completed.load(std::memory_order_acquire);
    if (!resumed && scheduler.has_single_ready_task()) {
        resumed = scheduler.run_one();
    }
    const bool completed = resumed &&
        state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        operation_result_matches(state, kind);
    const bool channelMatches = completed &&
        channel_matches_operation_result(channel, kind);

    scheduler.release_retained_state();
    return ownerBlocked && started && triggered && noEarlyWake && completed &&
        channelMatches &&
        taskState->m_refs.load(std::memory_order_acquire) == 1;
}

bool run_timed_out_waiter_does_not_swallow_resource(OperationKind kind)
{
    // timer 已裁决但任务尚未恢复时，队首 waiter 是 tombstone；scanner 必须
    // 越过它，把同一个资源事件交给后面的 live waiter。
    Channel channel(2);
    if (!prepare_channel(channel, kind)) {
        return false;
    }
    QueuedRaceScheduler scheduler;
    OperationState timedOutState;
    OperationState liveState;
    auto timedOutTask = run_with_timeout(&channel, kind, &timedOutState);
    auto liveTask = run_with_timeout(&channel, kind, &liveState);
    TaskRef timedOutKeeper = detail::TaskAccess::task_ref(timedOutTask);
    TaskRef liveKeeper = detail::TaskAccess::task_ref(liveTask);
    TaskState* timedOutTaskState = timedOutKeeper.state();
    TaskState* liveTaskState = liveKeeper.state();
    if (timedOutTaskState == nullptr || liveTaskState == nullptr) {
        return false;
    }

    TaskRef firstScheduled =
        detail::TaskAccess::detach_task(std::move(timedOutTask));
    TaskRef secondScheduled = detail::TaskAccess::detach_task(std::move(liveTask));
    const bool firstStarted =
        scheduler.schedule_immediately(std::move(firstScheduled));
    const bool secondStarted =
        scheduler.schedule_immediately(std::move(secondScheduled));
    const bool timerFired = scheduler.fire_timer(0);
    const bool timeoutReady = scheduler.ready_count() == 1;
    const bool triggered = trigger_operation(channel, kind);
    const bool bothReady = scheduler.ready_count() == 2;
    const bool firstResumed = scheduler.run_one();
    const bool secondResumed = bothReady && scheduler.run_one();
    const bool completed = timeout_result_matches(timedOutState) &&
        timedOutState.resumeCount.load(std::memory_order_acquire) == 1 &&
        operation_result_matches(liveState, kind) &&
        liveState.resumeCount.load(std::memory_order_acquire) == 1;
    const bool channelMatches = completed &&
        channel_matches_operation_result(channel, kind);

    if (!bothReady) {
        channel.close();
        while (scheduler.run_one()) {
        }
    }
    scheduler.release_retained_state();
    return firstStarted && secondStarted && timerFired && timeoutReady &&
        triggered && bothReady && firstResumed && secondResumed && completed &&
        channelMatches &&
        timedOutTaskState->m_refs.load(std::memory_order_acquire) == 1 &&
        liveTaskState->m_refs.load(std::memory_order_acquire) == 1;
}

}  // namespace

int main()
{
    if (!run_close_waits_for_reserved_producer()) {
        std::cerr << "[T161] close overtook reserved producer publication\n";
        return 1;
    }

    for (OperationKind kind : kOperationKinds) {
        if (!run_completion_before_awaiter_arm(kind)) {
            std::cerr << "[T161] " << operation_name(kind)
                      << " completion scheduled before awaiter arm\n";
            return 1;
        }
        if (!run_immediate_completion(kind, true)) {
            std::cerr << "[T161] " << operation_name(kind)
                      << " operation-win addTimer=false failed\n";
            return 1;
        }
        if (!run_immediate_completion(kind, false)) {
            std::cerr << "[T161] " << operation_name(kind)
                      << " timeout-win addTimer=false failed\n";
            return 1;
        }
        if (!run_operation_timer_arbitration(kind)) {
            std::cerr << "[T161] " << operation_name(kind)
                      << " operation-first dequeue-gap failed\n";
            return 1;
        }
        if (!run_timer_operation_arbitration(kind)) {
            std::cerr << "[T161] " << operation_name(kind)
                      << " timer-first preservation failed\n";
            return 1;
        }
        if (!run_close_timer_arbitration(kind, true)) {
            std::cerr << "[T161] " << operation_name(kind)
                      << " close-first arbitration failed\n";
            return 1;
        }
        if (!run_close_timer_arbitration(kind, false)) {
            std::cerr << "[T161] " << operation_name(kind)
                      << " timer-first close arbitration failed\n";
            return 1;
        }
        if (!run_pump_exit_work_race(kind)) {
            std::cerr << "[T161] " << operation_name(kind)
                      << " pump exit lost late work\n";
            return 1;
        }
        if (!run_timed_out_waiter_does_not_swallow_resource(kind)) {
            std::cerr << "[T161] " << operation_name(kind)
                      << " timed-out waiter swallowed resource\n";
            return 1;
        }
    }

    std::cout << "T161-MpscBoundedTimeoutRace PASS\n";
    return 0;
}
