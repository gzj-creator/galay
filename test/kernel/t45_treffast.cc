/**
 * @file t45_treffast.cc
 * @brief 用途：验证 `ComputeScheduler` 对 `TaskRef` 调度的快速路径。
 * 关键覆盖点：TaskRef 直接派发、快速入队、恢复执行与完成通知。
 * 通过条件：TaskRef 快速路径命中预期并返回 0。
 */

#include "kernel/kernel/compute_scheduler.h"
#include "kernel/kernel/task.h"

#include <atomic>
#include <concepts>
#include <iostream>
#include <type_traits>

using namespace galay::kernel;

namespace {

static_assert(std::same_as<decltype(ComputeTask{}.task), TaskRef>,
              "ComputeTask should carry TaskRef for fast-path scheduling");

std::atomic<int> g_completed{0};

Task<void> countingTask() {
    g_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

bool verifyTaskRefFastPath() {
    g_completed.store(0, std::memory_order_relaxed);

    ComputeScheduler scheduler;
    scheduler.start();

    Task<void> task = countingTask();
    detail::setTaskScheduler(detail::TaskAccess::taskRef(task), &scheduler);
    if (!scheduler.schedule(detail::TaskAccess::taskRef(task))) {
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
    if (!verifyTaskRefFastPath()) {
        return 1;
    }

    std::cout << "T45-ComputeSchedulerTaskRefFastPath PASS\n";
    return 0;
}
