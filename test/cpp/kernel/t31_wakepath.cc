/**
 * @file t31_wakepath.cc
 * @brief 用途：验证 `TaskCore` 在挂起、唤醒与恢复之间的核心调度路径。
 * 关键覆盖点：任务状态迁移、唤醒入口、continuation 恢复与执行收尾。
 * 通过条件：核心唤醒路径断言全部成立，测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/waker.h>

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
static_assert(std::is_same_v<decltype(IOReadyQueue{}.ready_lifo_slot),
                             std::optional<detail::ReadyEntry>>,
              "IOReadyQueue::ready_lifo_slot must store ReadyEntry");
static_assert(std::is_same_v<decltype(IOReadyQueue{}.local_ring), ChaseLevTaskRing>,
              "IOReadyQueue::local_ring must store the fixed-capacity Chase-Lev ring");
static_assert(std::is_same_v<decltype(IOReadyQueue{}.ready_inject_queue),
                             moodycamel::ConcurrentQueue<detail::ReadyEntry>>,
              "IOReadyQueue::ready_inject_queue must store ReadyEntry");
static_assert(std::is_same_v<decltype(IOReadyQueue{}.ready_inject_buffer),
                             std::vector<detail::ReadyEntry>>,
              "IOReadyQueue::ready_inject_buffer must store ReadyEntry");

namespace {

bool wait_until(const std::atomic<bool>& flag,
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

Task<void> same_thread_waiter(ManualWakeState* state) {
    co_await ManualSuspendAwaitable{state};
    state->resumed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

Task<void> same_thread_producer(ManualWakeState* state) {
    while (!state->armed.load(std::memory_order_acquire)) {
        co_yield true;
    }

    state->waker.wake_up();
    state->waker.wake_up();
    state->producer_done.store(true, std::memory_order_release);
    co_return;
}

bool run_same_thread_double_wake() {
    ManualWakeState state;
    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    runtime.start();

    auto* scheduler = runtime.get_next_io_scheduler();
    if (!scheduler) {
        std::cerr << "[T31] missing IO scheduler for same-thread wake test\n";
        runtime.stop();
        return false;
    }

    scheduler->schedule(detail::TaskAccess::detach_task(same_thread_waiter(&state)));
    scheduler->schedule(detail::TaskAccess::detach_task(same_thread_producer(&state)));

    const bool producer_done = wait_until(state.producer_done);

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

Task<void> wait_child(WaitChainState* state) {
    state->child_steps.fetch_add(1, std::memory_order_relaxed);
    co_yield true;
    state->child_steps.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

Task<void> wait_parent(WaitChainState* state) {
    auto child_result = co_await wait_child(state);
    assert(child_result.has_value());
    state->waiter_resumes.fetch_add(1, std::memory_order_relaxed);
    state->done.store(true, std::memory_order_release);
    co_return;
}

bool run_wait_chain_resume_once() {
    WaitChainState state;
    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    runtime.start();

    auto* scheduler = runtime.get_next_io_scheduler();
    if (!scheduler) {
        std::cerr << "[T31] missing IO scheduler for wait-chain test\n";
        runtime.stop();
        return false;
    }

    scheduler->schedule(detail::TaskAccess::detach_task(wait_parent(&state)));
    const bool done = wait_until(state.done);
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
    if (!run_same_thread_double_wake()) {
        return 1;
    }
    if (!run_wait_chain_resume_once()) {
        return 1;
    }

    std::cout << "T31-TaskCoreWakePath PASS\n";
    return 0;
}
