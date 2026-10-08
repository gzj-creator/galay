/**
 * @file t156_mpsc_timeout_race.cc
 * @brief 验证 MPSC recv timeout 在 inner waiter 发布后立即恢复时只完成一次。
 */

#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include "test/cpp/common/scheduler_test_adapter.h"
#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

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

struct BatchToState
{
    std::atomic<int> resumeCount{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> receivedValue{false};
    std::atomic<bool> receivedClosed{false};
    std::atomic<bool> receivedTimeout{false};
};

struct ReentrantCloseState
{
    std::atomic<int> resumeCount{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> receivedValue{false};
    std::atomic<bool> closeSucceeded{false};
};

enum class InlineSendKind : uint8_t {
    kCopySingle,
    kMoveSingle,
    kCopyBatch,
    kMoveBatch,
};

class ImmediateRaceScheduler final : public detail::SchedulerTestAdapter<ImmediateRaceScheduler>
{
public:
    explicit ImmediateRaceScheduler(galay::mpsc::UnboundedChannel<int>* channel,
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
        // 强制 WithTimeout 走 timeout_now()，覆盖 inner 发布后的同步恢复窗口。
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
    galay::mpsc::UnboundedChannel<int>* m_channel;
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

Task<void> receive_with_timeout(galay::mpsc::UnboundedChannel<int>* channel,
                              ReceiveState* state)
{
    auto result = co_await channel->recv().timeout(1h);
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    if (result.has_value()) {
        state->receivedValue.store(*result == 73, std::memory_order_release);
    } else {
        state->receivedTimeout.store(
            IOError::contains(result.error().code(), kTimeout),
            std::memory_order_release);
    }
    state->completed.store(true, std::memory_order_release);
    co_return;
}

Task<void> receive_batch_to(galay::mpsc::UnboundedChannel<int>* channel,
                          std::vector<int>* destination,
                          BatchToState* state)
{
    auto result = co_await channel->recv_batch_to(*destination, 4);
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    if (result.has_value()) {
        state->receivedValue.store(
            *result == 1 && destination->size() == 1 &&
                destination->front() == 73,
            std::memory_order_release);
    } else {
        state->receivedClosed.store(
            IOError::contains(result.error().code(), kClosed),
            std::memory_order_release);
    }
    state->completed.store(true, std::memory_order_release);
    co_return;
}

Task<void> receive_batch(galay::mpsc::UnboundedChannel<int>* channel,
                        BatchToState* state)
{
    auto result = co_await channel->recv_batch(4);
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    if (result.has_value()) {
        state->receivedValue.store(
            result->size() == 1 && result->front() == 73,
            std::memory_order_release);
    } else {
        state->receivedClosed.store(
            IOError::contains(result.error().code(), kClosed),
            std::memory_order_release);
    }
    state->completed.store(true, std::memory_order_release);
    co_return;
}

Task<void> receive_batch_to_with_timeout(
    galay::mpsc::UnboundedChannel<int>* channel,
    std::vector<int>* destination,
    BatchToState* state)
{
    auto result = co_await channel->recv_batch_to(*destination, 4).timeout(1h);
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    if (result.has_value()) {
        state->receivedValue.store(true, std::memory_order_release);
    } else {
        state->receivedTimeout.store(
            IOError::contains(result.error().code(), kTimeout),
            std::memory_order_release);
    }
    state->completed.store(true, std::memory_order_release);
    co_return;
}

Task<void> receive_then_close(galay::mpsc::UnboundedChannel<int>* channel,
                            ReentrantCloseState* state)
{
    auto result = co_await channel->recv();
    state->resumeCount.fetch_add(1, std::memory_order_relaxed);
    if (result.has_value()) {
        state->receivedValue.store(*result == 73, std::memory_order_release);
        state->closeSucceeded.store(channel->close(), std::memory_order_release);
    }
    state->completed.store(true, std::memory_order_release);
    co_return;
}

bool run_inline_wake_reentrant_close(InlineSendKind sendKind)
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    ImmediateRaceScheduler scheduler(&channel, false);
    ReentrantCloseState state;

    auto task = receive_then_close(&channel, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool suspended = !state.completed.load(std::memory_order_acquire);
    // schedule() 会在 send() 的调用栈内恢复 receiver；receiver 随即重入 close()。
    bool sent = false;
    switch (sendKind) {
    case InlineSendKind::kCopySingle: {
        const int value = 73;
        sent = channel.send(value);
        break;
    }
    case InlineSendKind::kMoveSingle:
        sent = channel.send(73);
        break;
    case InlineSendKind::kCopyBatch: {
        const std::vector<int> values{73};
        sent = channel.send_batch(values);
        break;
    }
    case InlineSendKind::kMoveBatch: {
        std::vector<int> values{73};
        sent = channel.send_batch(std::move(values));
        break;
    }
    }
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedValue.load(std::memory_order_acquire) &&
        state.closeSucceeded.load(std::memory_order_acquire);

    return started && suspended && sent && completed &&
        scheduler.schedule_calls() == 1 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 &&
        channel.is_closed_and_drained();
}

bool run_immediate_completion(bool publishValue)
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    ImmediateRaceScheduler scheduler(&channel, publishValue);
    ReceiveState state;

    auto task = receive_with_timeout(&channel, &state);
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

    const int expectedScheduleCalls = publishValue ? 1 : 0;
    return started && state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 && correctResult &&
        scheduler.add_timer_calls() == 1 &&
        scheduler.schedule_calls() == expectedScheduleCalls &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool run_producer_timer_arbitration()
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receive_with_timeout(&channel, &state);
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

bool run_timer_producer_arbitration()
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receive_with_timeout(&channel, &state);
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

bool run_batch_to_producer_wake()
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    QueuedRaceScheduler scheduler;
    BatchToState state;
    std::vector<int> destination;
    destination.reserve(4);

    auto task = receive_batch_to(&channel, &destination, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool suspended = !state.completed.load(std::memory_order_acquire);
    const bool sent = channel.send(73);
    const bool oneWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedValue.load(std::memory_order_acquire) &&
        !state.receivedClosed.load(std::memory_order_acquire) &&
        !state.receivedTimeout.load(std::memory_order_acquire);

    scheduler.release_retained_state();
    return started && suspended && sent && oneWake && resumed && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool run_batch_to_close_wake()
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    QueuedRaceScheduler scheduler;
    BatchToState state;
    std::vector<int> destination;
    destination.reserve(4);

    auto task = receive_batch_to(&channel, &destination, &state);
    TaskRef keeper = detail::TaskAccess::task_ref(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    const bool started = scheduler.schedule_immediately(std::move(scheduled));
    const bool suspended = !state.completed.load(std::memory_order_acquire);
    const bool closed = channel.close();
    const bool oneWake = scheduler.has_single_ready_task();
    const bool resumed = scheduler.run_one();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        !state.receivedValue.load(std::memory_order_acquire) &&
        state.receivedClosed.load(std::memory_order_acquire) &&
        !state.receivedTimeout.load(std::memory_order_acquire) &&
        destination.empty();

    scheduler.release_retained_state();
    return started && suspended && closed && oneWake && resumed && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 &&
        channel.is_closed_and_drained();
}

bool run_batch_to_timer_producer_arbitration()
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    QueuedRaceScheduler scheduler;
    BatchToState state;
    std::vector<int> destination;
    destination.reserve(4);

    auto task = receive_batch_to_with_timeout(&channel, &destination, &state);
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
        !state.receivedClosed.load(std::memory_order_acquire) &&
        state.receivedTimeout.load(std::memory_order_acquire) &&
        destination.empty();
    auto retainedValue = channel.try_recv();

    scheduler.release_retained_state();
    return started && timerFired && oneTimerWake && sent && stillOneWake &&
        resumed && timedOut && retainedValue.has_value() && *retainedValue == 73 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool run_first_activation_waiter_race()
{
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    constexpr size_t kIterations = 1'000;
#else
    constexpr size_t kIterations = 100'000;
#endif
#else
    constexpr size_t kIterations = 100'000;
#endif

    using Channel = galay::mpsc::UnboundedChannel<int>;
    std::barrier startRound(2);
    std::barrier finishRound(2);
    std::atomic<Channel*> current{nullptr};
    std::atomic<bool> sendSucceeded{false};

    std::thread producer([&]() {
        for (size_t iteration = 0; iteration < kIterations; ++iteration) {
            startRound.arrive_and_wait();
            Channel* channel = current.load(std::memory_order_acquire);
            if ((iteration & 1U) == 0) {
                std::this_thread::yield();
            }
            sendSucceeded.store(
                channel != nullptr && channel->send(73),
                std::memory_order_release);
            finishRound.arrive_and_wait();
        }
    });

    bool passed = true;
    for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        Channel channel(64, 0);
        QueuedRaceScheduler scheduler;
        BatchToState state;
        auto task = receive_batch(&channel, &state);
        TaskRef keeper = detail::TaskAccess::task_ref(task);
        TaskState* taskState = keeper.state();
        TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));

        current.store(&channel, std::memory_order_release);
        startRound.arrive_and_wait();
        if ((iteration & 1U) != 0) {
            std::this_thread::yield();
        }
        const bool started = scheduler.schedule_immediately(std::move(scheduled));
        finishRound.arrive_and_wait();

        bool resumed = state.completed.load(std::memory_order_acquire);
        if (!resumed && scheduler.has_single_ready_task()) {
            resumed = scheduler.run_one();
        }
        const bool completed = state.completed.load(std::memory_order_acquire) &&
            state.resumeCount.load(std::memory_order_acquire) == 1 &&
            state.receivedValue.load(std::memory_order_acquire) &&
            !state.receivedClosed.load(std::memory_order_acquire) &&
            !state.receivedTimeout.load(std::memory_order_acquire);

        if (!resumed || !completed) {
            passed = false;
            const bool closed = channel.close();
            if (closed && scheduler.has_single_ready_task()) {
                [[maybe_unused]] const bool ranRecovery = scheduler.run_one();
            }
        }
        if (taskState == nullptr || !started ||
            !sendSucceeded.load(std::memory_order_acquire) || !completed ||
            taskState->m_refs.load(std::memory_order_acquire) != 1 ||
            !channel.empty()) {
            passed = false;
        }
        scheduler.release_retained_state();
        current.store(nullptr, std::memory_order_release);
    }

    producer.join();
    return passed;
}

bool run_activated_token_stream_waiter_race()
{
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    constexpr size_t kIterations = 300;
#else
    constexpr size_t kIterations = 30'000;
#endif
#else
    constexpr size_t kIterations = 30'000;
#endif

    using Channel = galay::mpsc::UnboundedChannel<int>;
    std::barrier startRound(2);
    std::barrier finishRound(2);
    std::atomic<Channel*> currentChannel{nullptr};
    std::atomic<Channel::ProducerToken*> currentToken{nullptr};
    std::atomic<bool> sendSucceeded{false};

    std::thread producer([&]() {
        for (size_t iteration = 0; iteration < kIterations; ++iteration) {
            startRound.arrive_and_wait();
            Channel* channel = currentChannel.load(std::memory_order_acquire);
            Channel::ProducerToken* token =
                currentToken.load(std::memory_order_acquire);
            if ((iteration & 2U) == 0) {
                std::this_thread::yield();
            }
            int value = 73;
            sendSucceeded.store(
                channel != nullptr && token != nullptr &&
                    channel->send(*token, std::move(value)),
                std::memory_order_release);
            finishRound.arrive_and_wait();
        }
    });

    bool passed = true;
    for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        Channel channel(64, 0);
        auto token = channel.make_producer_token();
        int primingValue = 17;
        const bool primed = token.valid() &&
            channel.send(token, std::move(primingValue));
        auto primedValue = channel.try_recv();
        const bool activatedAndDrained = primed && primedValue.has_value() &&
            *primedValue == 17 && channel.empty();

        QueuedRaceScheduler scheduler;
        ReceiveState singleState;
        BatchToState batchState;
        std::vector<int> destination;
        destination.reserve(4);
        Task<void> task;
        const size_t receiveKind = iteration % 3U;
        if (receiveKind == 0) {
            task = receive_with_timeout(&channel, &singleState);
        } else if (receiveKind == 1) {
            task = receive_batch(&channel, &batchState);
        } else {
            task = receive_batch_to(&channel, &destination, &batchState);
        }
        TaskRef keeper = detail::TaskAccess::task_ref(task);
        TaskState* taskState = keeper.state();
        TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));

        sendSucceeded.store(false, std::memory_order_relaxed);
        currentChannel.store(&channel, std::memory_order_release);
        currentToken.store(&token, std::memory_order_release);
        startRound.arrive_and_wait();
        if ((iteration & 2U) != 0) {
            std::this_thread::yield();
        }
        const bool started = scheduler.schedule_immediately(std::move(scheduled));
        finishRound.arrive_and_wait();

        bool resumed = receiveKind == 0
            ? singleState.completed.load(std::memory_order_acquire)
            : batchState.completed.load(std::memory_order_acquire);
        if (!resumed && scheduler.has_single_ready_task()) {
            resumed = scheduler.run_one();
        }
        const bool completed = receiveKind == 0
            ? singleState.completed.load(std::memory_order_acquire) &&
                singleState.resumeCount.load(std::memory_order_acquire) == 1 &&
                singleState.receivedValue.load(std::memory_order_acquire) &&
                !singleState.receivedTimeout.load(std::memory_order_acquire)
            : batchState.completed.load(std::memory_order_acquire) &&
                batchState.resumeCount.load(std::memory_order_acquire) == 1 &&
                batchState.receivedValue.load(std::memory_order_acquire) &&
                !batchState.receivedClosed.load(std::memory_order_acquire) &&
                !batchState.receivedTimeout.load(std::memory_order_acquire);

        if (!resumed || !completed) {
            passed = false;
            const bool closed = channel.close();
            if (closed && scheduler.has_single_ready_task()) {
                const bool ranRecovery = scheduler.run_one();
                if (!ranRecovery) {
                    passed = false;
                }
            }
        }
        if (!activatedAndDrained || taskState == nullptr || !started ||
            !sendSucceeded.load(std::memory_order_acquire) || !completed ||
            taskState->m_refs.load(std::memory_order_acquire) != 1 ||
            !channel.empty()) {
            passed = false;
        }
        scheduler.release_retained_state();
        currentToken.store(nullptr, std::memory_order_release);
        currentChannel.store(nullptr, std::memory_order_release);
    }

    producer.join();
    return passed;
}

}  // namespace

int main()
{
    if (!run_inline_wake_reentrant_close(InlineSendKind::kCopySingle)) {
        std::cerr << "[T156] inline copy-single wake reentrant close failed\n";
        return 1;
    }
    if (!run_inline_wake_reentrant_close(InlineSendKind::kMoveSingle)) {
        std::cerr << "[T156] inline move-single wake reentrant close failed\n";
        return 1;
    }
    if (!run_inline_wake_reentrant_close(InlineSendKind::kCopyBatch)) {
        std::cerr << "[T156] inline copy-batch wake reentrant close failed\n";
        return 1;
    }
    if (!run_inline_wake_reentrant_close(InlineSendKind::kMoveBatch)) {
        std::cerr << "[T156] inline move-batch wake reentrant close failed\n";
        return 1;
    }
    if (!run_immediate_completion(true)) {
        std::cerr << "[T156] operation-win immediate resume failed\n";
        return 1;
    }
    if (!run_immediate_completion(false)) {
        std::cerr << "[T156] timeout-win immediate resume failed\n";
        return 1;
    }
    if (!run_producer_timer_arbitration()) {
        std::cerr << "[T156] producer/timer arbitration admitted a duplicate wake\n";
        return 1;
    }
    if (!run_timer_producer_arbitration()) {
        std::cerr << "[T156] timer/producer arbitration consumed a post-timeout value\n";
        return 1;
    }
    if (!run_batch_to_producer_wake()) {
        std::cerr << "[T156] recvBatchTo producer wake failed\n";
        return 1;
    }
    if (!run_batch_to_close_wake()) {
        std::cerr << "[T156] recvBatchTo close wake failed\n";
        return 1;
    }
    if (!run_batch_to_timer_producer_arbitration()) {
        std::cerr << "[T156] recvBatchTo timeout consumed a post-timeout value\n";
        return 1;
    }
    if (!run_first_activation_waiter_race()) {
        std::cerr << "[T156] first stream activation lost a waiter wake\n";
        return 1;
    }
    if (!run_activated_token_stream_waiter_race()) {
        std::cerr << "[T156] activated token stream lost a waiter wake\n";
        return 1;
    }

    std::cout << "T156-MpscTimeoutRace PASS\n";
    return 0;
}
