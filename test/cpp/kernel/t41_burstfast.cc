/**
 * @file t41_burstfast.cc
 * @brief 用途：验证注入任务突发到来时调度器的快速消费路径。
 * 关键覆盖点：burst 注入排队、远端队列批量 drain、完成统计与快速恢复。
 * 通过条件：突发注入任务都被正确消费，测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/task.h>
#include "test/cpp/common/sched_access.h"

#include <atomic>
#include <iostream>

using namespace galay::kernel;

namespace {

std::atomic<int> g_completed{0};

Task<void> counting_task() {
    g_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

template <typename SchedulerT>
bool verify_injected_burst_fast_path(const char* label) {
    constexpr int kTaskCount = 300;

    g_completed.store(0, std::memory_order_relaxed);
    SchedulerT scheduler;

    for (int i = 0; i < kTaskCount; ++i) {
        if (!scheduler.schedule(detail::TaskAccess::detach_task(counting_task()))) {
            std::cerr << "[T41] " << label << " failed to enqueue injected task " << i << "\n";
            return false;
        }
    }

    SchedulerTestAccess::process_pending(scheduler);

    const int completed_after_first_pass = g_completed.load(std::memory_order_relaxed);
    if (completed_after_first_pass != kTaskCount) {
        std::cerr << "[T41] " << label
                  << " should complete pure injected backlog in one pass, completed="
                  << completed_after_first_pass << "\n";
        return false;
    }

    return true;
}

bool verify_injected_burst_fast_path() {
#if defined(USE_KQUEUE)
    return verify_injected_burst_fast_path<KqueueScheduler>("kqueue");
#elif defined(USE_EPOLL)
    return verify_injected_burst_fast_path<EpollScheduler>("epoll");
#elif defined(USE_IOURING)
    return verify_injected_burst_fast_path<IOUringScheduler>("io_uring");
#else
    std::cout << "T41-SchedulerInjectedBurstFastPath SKIP\n";
    return true;
#endif
}

}  // namespace

int main() {
    if (!verify_injected_burst_fast_path()) {
        return 1;
    }

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    std::cout << "T41-SchedulerInjectedBurstFastPath PASS\n";
#endif
    return 0;
}
