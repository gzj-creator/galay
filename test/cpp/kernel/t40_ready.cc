/**
 * @file t40_ready.cc
 * @brief 用途：验证调度器每轮 ready 执行预算的限制与推进行为。
 * 关键覆盖点：单轮 ready budget 上限、剩余任务留待后续轮次、整体完成性。
 * 通过条件：执行预算行为与预期一致且测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/task.h>
#include "test/cpp/common/sched_access.h"

#include <atomic>
#include <iostream>

using namespace galay::kernel;

namespace {

std::atomic<int> g_completed{0};

Task<void> countingTask() {
    g_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

template <typename SchedulerT>
bool verifyLocalReadyBudget(const char* label) {
    constexpr int kTaskCount = 300;

    g_completed.store(0, std::memory_order_relaxed);
    SchedulerT scheduler;

    for (int i = 0; i < kTaskCount; ++i) {
        Task<void> task = countingTask();
        detail::setTaskScheduler(detail::TaskAccess::taskRef(task), &scheduler);
        SchedulerTestAccess::worker(scheduler).scheduleLocal(detail::TaskAccess::detachTask(std::move(task)));
        if (!SchedulerTestAccess::worker(scheduler).hasLocalWork()) {
            std::cerr << "[T40] " << label << " failed to enqueue local task " << i << "\n";
            return false;
        }
    }

    SchedulerTestAccess::processPending(scheduler);

    const int completed_after_first_pass = g_completed.load(std::memory_order_relaxed);
    if (completed_after_first_pass >= kTaskCount) {
        std::cerr << "[T40] " << label
                  << " should leave work for a later pass, completed="
                  << completed_after_first_pass << "\n";
        return false;
    }

    if (completed_after_first_pass <= 0) {
        std::cerr << "[T40] " << label << " did not execute any ready task\n";
        return false;
    }

    SchedulerTestAccess::processPending(scheduler);

    if (g_completed.load(std::memory_order_relaxed) != kTaskCount) {
        std::cerr << "[T40] " << label << " did not finish remaining ready tasks\n";
        return false;
    }

    return true;
}

bool verifyReadyBudget() {
#if defined(USE_KQUEUE)
    return verifyLocalReadyBudget<KqueueScheduler>("kqueue");
#elif defined(USE_EPOLL)
    return verifyLocalReadyBudget<EpollScheduler>("epoll");
#elif defined(USE_IOURING)
    return verifyLocalReadyBudget<IOUringScheduler>("io_uring");
#else
    std::cout << "T40-SchedulerReadyBudget SKIP\n";
    return true;
#endif
}

}  // namespace

int main() {
    if (!verifyReadyBudget()) {
        return 1;
    }

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    std::cout << "T40-SchedulerReadyBudget PASS\n";
#endif
    return 0;
}
