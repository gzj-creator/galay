/**
 * @file t27_localfirst.cc
 * @brief 用途：验证 IO 调度器优先处理本地 ready 队列再消费远端注入任务。
 * 关键覆盖点：本地任务优先级、远端注入排队、同轮与后续轮次的执行顺序。
 * 通过条件：观察到本地优先语义且断言成立，测试返回 0。
 */

#include "galay-kernel/concurrency/async_waiter.h"
#include "galay-kernel/common/timer_manager.hpp"
#include "galay-kernel/core/task.h"

#if defined(USE_KQUEUE)
#include "galay-kernel/core/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
static constexpr const char* kSchedulerName = "kqueue";
#elif defined(USE_IOURING)
#include "galay-kernel/core/uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
static constexpr const char* kSchedulerName = "io_uring";
#elif defined(USE_EPOLL)
#include "galay-kernel/core/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
static constexpr const char* kSchedulerName = "epoll";
#else
#error "T27-IOSchedulerLocalFirst requires kqueue, epoll, or io_uring"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

constexpr int kTraceWaiterArmed = 1;
constexpr int kTraceOldWorkRan = 2;
constexpr int kTraceWaiterResumed = 3;

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool waitUntil(const std::atomic<bool>& flag,
               std::chrono::milliseconds timeout = 500ms,
               std::chrono::milliseconds step = 2ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return flag.load(std::memory_order_acquire);
}

struct SameThreadWakeState {
    AsyncWaiter<void> waiter;
    std::array<int, 16> trace{};
    std::atomic<int> trace_size{0};
    std::atomic<bool> waiter_resumed{false};
    std::atomic<bool> old_work_ran{false};
};

void recordTrace(SameThreadWakeState* state, int value) {
    const int idx = state->trace_size.fetch_add(1, std::memory_order_acq_rel);
    if (idx >= 0 && idx < static_cast<int>(state->trace.size())) {
        state->trace[static_cast<size_t>(idx)] = value;
    }
}

int findTrace(const SameThreadWakeState& state, int value) {
    const int size = state.trace_size.load(std::memory_order_acquire);
    for (int i = 0; i < size && i < static_cast<int>(state.trace.size()); ++i) {
        if (state.trace[static_cast<size_t>(i)] == value) {
            return i;
        }
    }
    return -1;
}

Task<void> sameThreadWaiter(SameThreadWakeState* state) {
    recordTrace(state, kTraceWaiterArmed);
    auto result = co_await state->waiter.wait();
    if (result) {
        recordTrace(state, kTraceWaiterResumed);
        state->waiter_resumed.store(true, std::memory_order_release);
    }
    co_return;
}

Task<void> sameThreadOldWork(SameThreadWakeState* state) {
    recordTrace(state, kTraceOldWorkRan);
    state->old_work_ran.store(true, std::memory_order_release);
    co_return;
}

Task<void> sameThreadDriver(IOSchedulerType* scheduler, SameThreadWakeState* state) {
    if (!scheduleTask(scheduler, sameThreadWaiter(state))) {
        throw std::runtime_error("failed to schedule sameThreadWaiter");
    }

    // Let the waiter task run and suspend on AsyncWaiter before we enqueue old work.
    co_yield true;

    if (!scheduleTask(scheduler, sameThreadOldWork(state))) {
        throw std::runtime_error("failed to schedule sameThreadOldWork");
    }
    state->waiter.notify();
    co_return;
}

bool runSameThreadWakeScenario() {
    SameThreadWakeState state;
    IOSchedulerType scheduler;
    scheduler.start();
    if (!scheduleTask(scheduler, sameThreadDriver(&scheduler, &state))) {
        std::cerr << "[T27] failed to schedule sameThreadDriver\n";
        scheduler.stop();
        return false;
    }

    const bool waiter_done = waitUntil(state.waiter_resumed);
    const bool old_done = waitUntil(state.old_work_ran);
    scheduler.stop();

    if (!waiter_done || !old_done) {
        std::cerr << "[T27] same-thread wake scenario timed out\n";
        return false;
    }

    const int old_pos = findTrace(state, kTraceOldWorkRan);
    const int resumed_pos = findTrace(state, kTraceWaiterResumed);
    if (old_pos < 0 || resumed_pos < 0) {
        std::cerr << "[T27] same-thread wake trace incomplete\n";
        return false;
    }

    if (resumed_pos > old_pos) {
        std::cerr << "[T27] expected same-thread wake to run resumed task before older queued work"
                  << ", trace old_pos=" << old_pos
                  << " resumed_pos=" << resumed_pos << "\n";
        return false;
    }
    return true;
}

struct CrossThreadWakeState {
    std::atomic<bool> done{false};
    std::atomic<int64_t> submitted_ns{0};
    std::atomic<int64_t> executed_ns{0};
};

Task<void> crossThreadTask(CrossThreadWakeState* state) {
    state->executed_ns.store(nowNs(), std::memory_order_release);
    state->done.store(true, std::memory_order_release);
    co_return;
}

bool runCrossThreadWakeScenario() {
    CrossThreadWakeState state;
    IOSchedulerType scheduler;
    scheduler.replaceTimerManager(TimingWheelTimerManager(200000000ULL));
    scheduler.start();

    // Give the worker time to enter its parked wait path.
    std::this_thread::sleep_for(60ms);

    state.submitted_ns.store(nowNs(), std::memory_order_release);
    if (!scheduleTask(scheduler, crossThreadTask(&state))) {
        std::cerr << "[T27] failed to schedule crossThreadTask\n";
        scheduler.stop();
        return false;
    }

    const bool done = waitUntil(state.done, 300ms);
    scheduler.stop();

    if (!done) {
        std::cerr << "[T27] cross-thread wake scenario timed out\n";
        return false;
    }

    const int64_t latency_ns =
        state.executed_ns.load(std::memory_order_acquire) -
        state.submitted_ns.load(std::memory_order_acquire);
    const auto latency_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::nanoseconds(latency_ns)).count();

    if (latency_ms > 40) {
        std::cerr << "[T27] expected cross-thread task submission to wake parked " << kSchedulerName
                  << " worker quickly, latency_ms=" << latency_ms << "\n";
        return false;
    }

    return true;
}

struct FairnessState {
    std::atomic<bool> old_seen{false};
    std::atomic<bool> hot_done{false};
    std::atomic<bool> old_after_hot{false};
};

Task<void> fairnessOldTask(FairnessState* state) {
    if (state->hot_done.load(std::memory_order_acquire)) {
        state->old_after_hot.store(true, std::memory_order_release);
    }
    state->old_seen.store(true, std::memory_order_release);
    co_return;
}

Task<void> fairnessHotTask(FairnessState* state, int rounds) {
    for (int i = 0; i < rounds; ++i) {
        co_yield true;
    }
    state->hot_done.store(true, std::memory_order_release);
    co_return;
}

Task<void> fairnessDriver(IOSchedulerType* scheduler, FairnessState* state) {
    if (!scheduleTask(scheduler, fairnessHotTask(state, 32))) {
        throw std::runtime_error("failed to schedule fairnessHotTask");
    }
    if (!scheduleTask(scheduler, fairnessOldTask(state))) {
        throw std::runtime_error("failed to schedule fairnessOldTask");
    }
    co_return;
}

bool runFairnessScenario() {
    FairnessState state;
    IOSchedulerType scheduler;
    scheduler.start();
    if (!scheduleTask(scheduler, fairnessDriver(&scheduler, &state))) {
        std::cerr << "[T27] failed to schedule fairnessDriver\n";
        scheduler.stop();
        return false;
    }

    const bool old_done = waitUntil(state.old_seen);
    const bool hot_done = waitUntil(state.hot_done);
    scheduler.stop();

    if (!old_done || !hot_done) {
        std::cerr << "[T27] fairness scenario timed out\n";
        return false;
    }

    if (state.old_after_hot.load(std::memory_order_acquire)) {
        std::cerr << "[T27] expected older queued work to run before hot same-thread reschedule chain completed\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!runSameThreadWakeScenario()) {
        return 1;
    }
    if (!runCrossThreadWakeScenario()) {
        return 1;
    }
    if (!runFairnessScenario()) {
        return 1;
    }

    std::cout << "T27-IOSchedulerLocalFirst PASS\n";
    return 0;
}
