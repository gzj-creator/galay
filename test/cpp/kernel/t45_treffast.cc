/**
 * @file t45_treffast.cc
 * @brief 用途：验证 `ParallelScheduler` 对 `TaskRef` 调度的快速路径。
 * 关键覆盖点：TaskRef 直接派发、快速入队、恢复执行与完成通知。
 * 通过条件：TaskRef 快速路径命中预期并返回 0。
 */

#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <concepts>
#include <iostream>
#include <type_traits>

using namespace galay::kernel;

namespace {

static_assert(std::same_as<decltype(ParallelTask{}.task), TaskRef>,
              "ParallelTask should carry TaskRef for fast-path scheduling");

std::atomic<int> g_completed{0};

Task<void> counting_task() {
    g_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

bool verify_task_ref_fast_path() {
    g_completed.store(0, std::memory_order_relaxed);

    ParallelScheduler scheduler;
    scheduler.start();

    Task<void> task = counting_task();
    detail::set_task_scheduler(detail::TaskAccess::task_ref(task), &scheduler);
    if (!scheduler.schedule(detail::TaskAccess::task_ref(task))) {
        std::cerr << "[T45] schedule(TaskRef) rejected valid compute task\n";
        scheduler.stop();
        return false;
    }

    scheduler.stop();

    if (g_completed.load(std::memory_order_relaxed) != 1) {
        std::cerr << "[T45] expected compute task to complete once\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!verify_task_ref_fast_path()) {
        return 1;
    }

    std::cout << "T45-ParallelSchedulerTaskRefFastPath PASS\n";
    return 0;
}
