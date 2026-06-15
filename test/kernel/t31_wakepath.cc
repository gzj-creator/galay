/**
 * @file t31_wakepath.cc
 * @brief 用途：验证 `TaskCore` 在挂起、唤醒与恢复之间的核心调度路径。
 * 关键覆盖点：任务状态迁移、唤醒入口、continuation 恢复与执行收尾。
 * 通过条件：核心唤醒路径断言全部成立，测试返回 0。
 */

#include "galay-kernel/core/io_scheduler.hpp"
#include "galay-kernel/core/runtime.h"
#include "galay-kernel/core/task.h"
#include "galay-kernel/core/waker.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <thread>
#include <type_traits>

using namespace galay::kernel;
using namespace std::chrono_literals;

static_assert(sizeof(TaskRef) == sizeof(void*),
              "TaskRef must stay pointer-sized once the lightweight task core lands");
static_assert(sizeof(Waker) == sizeof(void*),
              "Waker must stay pointer-sized once the lightweight task core lands");
static_assert(std::is_constructible_v<Waker, TaskRef>,
              "Waker must capture lightweight task refs directly");
static_assert(std::is_same_v<decltype(IOSchedulerWorkerState{}.lifo_slot), std::optional<TaskRef>>,
              "IOSchedulerWorkerState::lifo_slot must store TaskRef");
static_assert(std::is_same_v<decltype(IOSchedulerWorkerState{}.local_ring), ChaseLevTaskRing>,
              "IOSchedulerWorkerState::local_ring must store the fixed-capacity Chase-Lev ring");
static_assert(std::is_same_v<decltype(IOSchedulerWorkerState{}.inject_queue),
                             moodycamel::ConcurrentQueue<TaskRef>>,
              "IOSchedulerWorkerState::inject_queue must store TaskRef");
static_assert(std::is_same_v<decltype(IOSchedulerWorkerState{}.inject_buffer), std::vector<TaskRef>>,
              "IOSchedulerWorkerState::inject_buffer must store TaskRef");

namespace {

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

struct ManualWakeState {
    Waker waker;
    std::atomic<bool> armed{false};
    std::atomic<bool> producer_done{false};
    std::atomic<int> resumed{0};
};

struct ManualSuspendAwaitable {
    ManualWakeState* state;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        state->waker = Waker(handle);
        state->armed.store(true, std::memory_order_release);
        return true;
    }

    void await_resume() const noexcept {}
};

Task<void> sameThreadWaiter(ManualWakeState* state) {
    co_await ManualSuspendAwaitable{state};
    state->resumed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

Task<void> sameThreadProducer(ManualWakeState* state) {
    while (!state->armed.load(std::memory_order_acquire)) {
        co_yield true;
    }

    state->waker.wakeUp();
    state->waker.wakeUp();
    state->producer_done.store(true, std::memory_order_release);
    co_return;
}

bool runSameThreadDoubleWake() {
    ManualWakeState state;
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "[T31] missing IO scheduler for same-thread wake test\n";
        runtime.stop();
        return false;
    }

    scheduler->schedule(detail::TaskAccess::detachTask(sameThreadWaiter(&state)));
    scheduler->schedule(detail::TaskAccess::detachTask(sameThreadProducer(&state)));

    const bool producer_done = waitUntil(state.producer_done);

    const bool waiter_done = [&state]() {
        const auto deadline = std::chrono::steady_clock::now() + 500ms;
        while (std::chrono::steady_clock::now() < deadline) {
            if (state.resumed.load(std::memory_order_acquire) == 1) {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return state.resumed.load(std::memory_order_acquire) == 1;
    }();

    runtime.stop();

    if (!producer_done || !waiter_done) {
        std::cerr << "[T31] same-thread wake test timed out, resumed="
                  << state.resumed.load(std::memory_order_acquire) << "\n";
        return false;
    }

    if (state.resumed.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T31] expected double wake to resume suspended task once, got resumed="
                  << state.resumed.load(std::memory_order_acquire) << "\n";
        return false;
    }

    return true;
}

struct WaitChainState {
    std::atomic<int> child_steps{0};
    std::atomic<int> waiter_resumes{0};
    std::atomic<bool> done{false};
};

Task<void> waitChild(WaitChainState* state) {
    state->child_steps.fetch_add(1, std::memory_order_relaxed);
    co_yield true;
    state->child_steps.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

Task<void> waitParent(WaitChainState* state) {
    auto child_result = co_await waitChild(state);
    assert(child_result.has_value());
    state->waiter_resumes.fetch_add(1, std::memory_order_relaxed);
    state->done.store(true, std::memory_order_release);
    co_return;
}

bool runWaitChainResumeOnce() {
    WaitChainState state;
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "[T31] missing IO scheduler for wait-chain test\n";
        runtime.stop();
        return false;
    }

    scheduler->schedule(detail::TaskAccess::detachTask(waitParent(&state)));
    const bool done = waitUntil(state.done);
    runtime.stop();

    if (!done) {
        std::cerr << "[T31] wait-chain test timed out\n";
        return false;
    }

    if (state.child_steps.load(std::memory_order_acquire) != 2 ||
        state.waiter_resumes.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T31] expected child_steps=2 waiter_resumes=1, got child_steps="
                  << state.child_steps.load(std::memory_order_acquire)
                  << " waiter_resumes=" << state.waiter_resumes.load(std::memory_order_acquire)
                  << "\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!runSameThreadDoubleWake()) {
        return 1;
    }
    if (!runWaitChainResumeOnce()) {
        return 1;
    }

    std::cout << "T31-TaskCoreWakePath PASS\n";
    return 0;
}
