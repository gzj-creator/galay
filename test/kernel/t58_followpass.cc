/**
 * @file t58_followpass.cc
 * @brief 用途：验证调度器在本地 follow-up 任务存在时会继续推进后续轮次。
 * 关键覆盖点：本地 follow-up 任务产生、额外 pass 推进、避免过早进入 poll。
 * 通过条件：follow-up 任务在预期轮次内被继续处理，测试返回 0。
 */

#include "kernel/kernel/scheduler_core.h"
#include "kernel/kernel/task.h"

#include <iostream>

using namespace galay::kernel;

namespace {

Task<void> pendingTask() {
    co_return;
}

bool verifyLocalFollowupPassesDrainLocalBacklog() {
    constexpr size_t kReadyBudget = 64;
    constexpr size_t kTaskCount = 200;

    IOSchedulerWorkerState worker(kReadyBudget);
    SchedulerCore core(worker, kReadyBudget);
    size_t resumed = 0;

    for (size_t i = 0; i < kTaskCount; ++i) {
        worker.scheduleLocal(detail::TaskAccess::detachTask(pendingTask()));
    }

    const auto summary = core.runLocalFollowupPasses(
        8,
        [&](TaskRef&) { ++resumed; },
        [](size_t) {});

    if (summary.ran != kTaskCount || resumed != kTaskCount) {
        std::cerr << "[T58] local follow-up passes should drain full local backlog, ran="
                  << summary.ran << ", resumed=" << resumed << "\n";
        return false;
    }

    if (summary.drainedRemote != 0) {
        std::cerr << "[T58] local backlog should not report remote drains, drained="
                  << summary.drainedRemote << "\n";
        return false;
    }

    if (summary.passes <= 1) {
        std::cerr << "[T58] local backlog should require multiple passes when over budget, passes="
                  << summary.passes << "\n";
        return false;
    }

    if (worker.hasLocalWork()) {
        std::cerr << "[T58] local backlog should be empty after follow-up passes\n";
        return false;
    }

    return true;
}

bool verifyRemoteDrainDoesNotNeedFollowupPasses() {
    constexpr size_t kReadyBudget = 64;
    constexpr size_t kTaskCount = 300;

    IOSchedulerWorkerState worker(kReadyBudget);
    SchedulerCore core(worker, kReadyBudget);
    size_t resumed = 0;
    size_t remote_drained = 0;

    for (size_t i = 0; i < kTaskCount; ++i) {
        worker.scheduleInjected(detail::TaskAccess::detachTask(pendingTask()));
    }

    const auto summary = core.runLocalFollowupPasses(
        8,
        [&](TaskRef&) { ++resumed; },
        [&](size_t drained) { remote_drained += drained; });

    if (summary.ran != kTaskCount || resumed != kTaskCount) {
        std::cerr << "[T58] injected backlog should still complete, ran="
                  << summary.ran << ", resumed=" << resumed << "\n";
        return false;
    }

    if (summary.drainedRemote != kTaskCount || remote_drained != kTaskCount) {
        std::cerr << "[T58] injected backlog should report one remote drain of full backlog, summary="
                  << summary.drainedRemote << ", callback=" << remote_drained << "\n";
        return false;
    }

    if (summary.passes != 1) {
        std::cerr << "[T58] injected burst fast-path should complete in one pass, passes="
                  << summary.passes << "\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!verifyLocalFollowupPassesDrainLocalBacklog()) {
        return 1;
    }

    if (!verifyRemoteDrainDoesNotNeedFollowupPasses()) {
        return 1;
    }

    std::cout << "T58-SchedulerLocalFollowupPasses PASS\n";
    return 0;
}
