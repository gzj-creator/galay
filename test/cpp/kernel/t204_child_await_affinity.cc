/**
 * @file t204_child_await_affinity.cc
 * @brief 复现预绑定子任务在错误线程恢复的已知缺陷。
 *
 * @details 子任务先绑定调度器 B、尚未启动，再由调度器 A 上的父任务
 * `co_await`。正确行为是子任务体只在 B 的线程执行；当前实现会在 A 的线程
 * 直接调用 B 的 `schedule_immediately`，因此本测试当前失败。生产修复后移除
 * CMake 中的 DISABLED 属性，使该用例进入常规回归。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

/** @brief 记录父任务和子任务的执行线程。 */
struct ChildAffinityState {
    std::atomic<bool> done{false};  ///< 父任务是否完成等待
    std::atomic<bool> await_ok{false};  ///< 子任务是否成功返回
    std::thread::id parent_thread;  ///< 父任务执行线程
    std::thread::id child_thread;  ///< 子任务执行线程
};

/**
 * @brief 等待给定条件，直至超时。
 * @param predicate 条件回调
 * @param timeout 最长等待时间
 * @return 条件在超时前满足时返回 true
 */
bool wait_until(auto&& predicate, std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

/** @brief 记录执行线程后立即完成的子任务。 */
Task<void> affinity_child(ChildAffinityState* state)
{
    state->child_thread = std::this_thread::get_id();
    co_return;
}

/** @brief 把子任务预先绑定到另一个调度器后再等待。 */
Task<void> affinity_parent(ChildAffinityState* state, IOScheduler* child_scheduler)
{
    state->parent_thread = std::this_thread::get_id();
    auto child = affinity_child(state);
    detail::set_task_scheduler(detail::TaskAccess::task_ref(child), child_scheduler);
    const auto result = co_await child;
    state->await_ok.store(result.has_value(), std::memory_order_release);
    state->done.store(true, std::memory_order_release);
    co_return;
}

/**
 * @brief 执行跨调度器预绑定复现场景。
 * @return 子任务在预绑定调度器线程执行且父任务正常完成时返回 true
 */
bool run_affinity_scenario()
{
    Runtime runtime{RuntimeConfig{.io_scheduler_count = 2, .parallel_scheduler_count = 0}};
    const auto started = runtime.start();
    if (!started) {
        std::cerr << "[T204] runtime start failed\n";
        return false;
    }

    IOScheduler* const parent_scheduler = runtime.get_io_scheduler(0);
    IOScheduler* const child_scheduler = runtime.get_io_scheduler(1);
    if (parent_scheduler == nullptr || child_scheduler == nullptr) {
        std::cerr << "[T204] missing IO scheduler\n";
        runtime.stop();
        return false;
    }

    ChildAffinityState state;
    if (!schedule_task(*parent_scheduler, affinity_parent(&state, child_scheduler))) {
        std::cerr << "[T204] failed to schedule parent task\n";
        runtime.stop();
        return false;
    }

    const bool completed = wait_until([&]() {
        return state.done.load(std::memory_order_acquire);
    });
    runtime.stop();

    if (!completed) {
        std::cerr << "[T204] parent task did not complete\n";
        return false;
    }

    const bool parent_on_owner = state.parent_thread == parent_scheduler->thread_id();
    const bool child_on_owner = state.child_thread == child_scheduler->thread_id();
    if (!parent_on_owner || !child_on_owner ||
        !state.await_ok.load(std::memory_order_acquire)) {
        std::cerr << "[T204] affinity mismatch: parent_on_owner=" << parent_on_owner
                  << " child_on_owner=" << child_on_owner
                  << " await_ok=" << state.await_ok.load(std::memory_order_acquire)
                  << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    return run_affinity_scenario() ? 0 : 1;
}
