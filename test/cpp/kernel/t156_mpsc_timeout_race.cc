/**
 * @file t156_mpsc_timeout_race.cc
 * @brief 验证 MPSC recv timeout 在 inner waiter 发布后立即恢复时只完成一次。
 */

#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/core/scheduler.hpp>
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

class ImmediateRaceScheduler final : public Scheduler
{
public:
    explicit ImmediateRaceScheduler(galay::mpsc::UnboundedChannel<int>* channel,
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
        // 强制 WithTimeout 走 timeoutNow()，覆盖 inner 发布后的同步恢复窗口。
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
    galay::mpsc::UnboundedChannel<int>* m_channel;
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

Task<void> receiveWithTimeout(galay::mpsc::UnboundedChannel<int>* channel,
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

Task<void> receiveBatchTo(galay::mpsc::UnboundedChannel<int>* channel,
                          std::vector<int>* destination,
                          BatchToState* state)
{
    auto result = co_await channel->recvBatchTo(*destination, 4);
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

Task<void> receiveBatch(galay::mpsc::UnboundedChannel<int>* channel,
                        BatchToState* state)
{
    auto result = co_await channel->recvBatch(4);
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

Task<void> receiveBatchToWithTimeout(
    galay::mpsc::UnboundedChannel<int>* channel,
    std::vector<int>* destination,
    BatchToState* state)
{
    auto result = co_await channel->recvBatchTo(*destination, 4).timeout(1h);
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

Task<void> receiveThenClose(galay::mpsc::UnboundedChannel<int>* channel,
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

bool runInlineWakeReentrantClose(InlineSendKind sendKind)
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    ImmediateRaceScheduler scheduler(&channel, false);
    ReentrantCloseState state;

    auto task = receiveThenClose(&channel, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
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
        sent = channel.sendBatch(values);
        break;
    }
    case InlineSendKind::kMoveBatch: {
        std::vector<int> values{73};
        sent = channel.sendBatch(std::move(values));
        break;
    }
    }
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedValue.load(std::memory_order_acquire) &&
        state.closeSucceeded.load(std::memory_order_acquire);

    return started && suspended && sent && completed &&
        scheduler.scheduleCalls() == 1 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 &&
        channel.isClosedAndDrained();
}

bool runImmediateCompletion(bool publishValue)
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    ImmediateRaceScheduler scheduler(&channel, publishValue);
    ReceiveState state;

    auto task = receiveWithTimeout(&channel, &state);
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

    const int expectedScheduleCalls = publishValue ? 1 : 0;
    return started && state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 && correctResult &&
        scheduler.addTimerCalls() == 1 &&
        scheduler.scheduleCalls() == expectedScheduleCalls &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool runProducerTimerArbitration()
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receiveWithTimeout(&channel, &state);
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

bool runTimerProducerArbitration()
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    QueuedRaceScheduler scheduler;
    ReceiveState state;

    auto task = receiveWithTimeout(&channel, &state);
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

bool runBatchToProducerWake()
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    QueuedRaceScheduler scheduler;
    BatchToState state;
    std::vector<int> destination;
    destination.reserve(4);

    auto task = receiveBatchTo(&channel, &destination, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool suspended = !state.completed.load(std::memory_order_acquire);
    const bool sent = channel.send(73);
    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        state.receivedValue.load(std::memory_order_acquire) &&
        !state.receivedClosed.load(std::memory_order_acquire) &&
        !state.receivedTimeout.load(std::memory_order_acquire);

    scheduler.releaseRetainedState();
    return started && suspended && sent && oneWake && resumed && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool runBatchToCloseWake()
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    QueuedRaceScheduler scheduler;
    BatchToState state;
    std::vector<int> destination;
    destination.reserve(4);

    auto task = receiveBatchTo(&channel, &destination, &state);
    TaskRef keeper = detail::TaskAccess::taskRef(task);
    TaskState* taskState = keeper.state();
    if (taskState == nullptr) {
        return false;
    }

    TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));
    const bool started = scheduler.scheduleImmediately(std::move(scheduled));
    const bool suspended = !state.completed.load(std::memory_order_acquire);
    const bool closed = channel.close();
    const bool oneWake = scheduler.hasSingleReadyTask();
    const bool resumed = scheduler.runOne();
    const bool completed = state.completed.load(std::memory_order_acquire) &&
        state.resumeCount.load(std::memory_order_acquire) == 1 &&
        !state.receivedValue.load(std::memory_order_acquire) &&
        state.receivedClosed.load(std::memory_order_acquire) &&
        !state.receivedTimeout.load(std::memory_order_acquire) &&
        destination.empty();

    scheduler.releaseRetainedState();
    return started && suspended && closed && oneWake && resumed && completed &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 &&
        channel.isClosedAndDrained();
}

bool runBatchToTimerProducerArbitration()
{
    galay::mpsc::UnboundedChannel<int> channel(64, 0);
    QueuedRaceScheduler scheduler;
    BatchToState state;
    std::vector<int> destination;
    destination.reserve(4);

    auto task = receiveBatchToWithTimeout(&channel, &destination, &state);
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
        !state.receivedClosed.load(std::memory_order_acquire) &&
        state.receivedTimeout.load(std::memory_order_acquire) &&
        destination.empty();
    auto retainedValue = channel.tryRecv();

    scheduler.releaseRetainedState();
    return started && timerFired && oneTimerWake && sent && stillOneWake &&
        resumed && timedOut && retainedValue.has_value() && *retainedValue == 73 &&
        taskState->m_refs.load(std::memory_order_acquire) == 1 && channel.empty();
}

bool runFirstActivationWaiterRace()
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
        auto task = receiveBatch(&channel, &state);
        TaskRef keeper = detail::TaskAccess::taskRef(task);
        TaskState* taskState = keeper.state();
        TaskRef scheduled = detail::TaskAccess::detachTask(std::move(task));

        current.store(&channel, std::memory_order_release);
        startRound.arrive_and_wait();
        if ((iteration & 1U) != 0) {
            std::this_thread::yield();
        }
        const bool started = scheduler.scheduleImmediately(std::move(scheduled));
        finishRound.arrive_and_wait();

        bool resumed = state.completed.load(std::memory_order_acquire);
        if (!resumed && scheduler.hasSingleReadyTask()) {
            resumed = scheduler.runOne();
        }
        const bool completed = state.completed.load(std::memory_order_acquire) &&
            state.resumeCount.load(std::memory_order_acquire) == 1 &&
            state.receivedValue.load(std::memory_order_acquire) &&
            !state.receivedClosed.load(std::memory_order_acquire) &&
            !state.receivedTimeout.load(std::memory_order_acquire);

        if (!resumed || !completed) {
            passed = false;
            const bool closed = channel.close();
            if (closed && scheduler.hasSingleReadyTask()) {
                [[maybe_unused]] const bool ranRecovery = scheduler.runOne();
            }
        }
        if (taskState == nullptr || !started ||
            !sendSucceeded.load(std::memory_order_acquire) || !completed ||
            taskState->m_refs.load(std::memory_order_acquire) != 1 ||
            !channel.empty()) {
            passed = false;
        }
        scheduler.releaseRetainedState();
        current.store(nullptr, std::memory_order_release);
    }

    producer.join();
    return passed;
}

}  // namespace

int main()
{
    if (!runInlineWakeReentrantClose(InlineSendKind::kCopySingle)) {
        std::cerr << "[T156] inline copy-single wake reentrant close failed\n";
        return 1;
    }
    if (!runInlineWakeReentrantClose(InlineSendKind::kMoveSingle)) {
        std::cerr << "[T156] inline move-single wake reentrant close failed\n";
        return 1;
    }
    if (!runInlineWakeReentrantClose(InlineSendKind::kCopyBatch)) {
        std::cerr << "[T156] inline copy-batch wake reentrant close failed\n";
        return 1;
    }
    if (!runInlineWakeReentrantClose(InlineSendKind::kMoveBatch)) {
        std::cerr << "[T156] inline move-batch wake reentrant close failed\n";
        return 1;
    }
    if (!runImmediateCompletion(true)) {
        std::cerr << "[T156] operation-win immediate resume failed\n";
        return 1;
    }
    if (!runImmediateCompletion(false)) {
        std::cerr << "[T156] timeout-win immediate resume failed\n";
        return 1;
    }
    if (!runProducerTimerArbitration()) {
        std::cerr << "[T156] producer/timer arbitration admitted a duplicate wake\n";
        return 1;
    }
    if (!runTimerProducerArbitration()) {
        std::cerr << "[T156] timer/producer arbitration consumed a post-timeout value\n";
        return 1;
    }
    if (!runBatchToProducerWake()) {
        std::cerr << "[T156] recvBatchTo producer wake failed\n";
        return 1;
    }
    if (!runBatchToCloseWake()) {
        std::cerr << "[T156] recvBatchTo close wake failed\n";
        return 1;
    }
    if (!runBatchToTimerProducerArbitration()) {
        std::cerr << "[T156] recvBatchTo timeout consumed a post-timeout value\n";
        return 1;
    }
    if (!runFirstActivationWaiterRace()) {
        std::cerr << "[T156] first stream activation lost a waiter wake\n";
        return 1;
    }

    std::cout << "T156-MpscTimeoutRace PASS\n";
    return 0;
}
