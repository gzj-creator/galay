/**
 * @file sched_loop.hpp
 * @brief 共用的 IO 调度器事件循环骨架
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供三个 IO 调度器后端（epoll、kqueue、io_uring）共用的通用事件循环模板。
 * 循环交替执行：排空就绪任务、驱动时间轮 tick、轮询后端完成事件。
 */

#ifndef GALAY_KERNEL_IOSCHEDULER_EVENT_LOOP_HPP
#define GALAY_KERNEL_IOSCHEDULER_EVENT_LOOP_HPP

#include "scheduler_core.h"
#include "wake_coordinator.h"
#include "galay-kernel/common/timer_manager.hpp"

#include <atomic>
#include <cstddef>
#include <utility>

namespace galay::kernel {

namespace detail {

/**
 * @brief 处理跨线程注入与本地 ready 队列（三后端共用）
 * @param resume_fn 须由具体调度器传入，以便在派生类上下文中调用受保护的 `Scheduler::resume`
 */
template <typename ResumeFn>
inline void ioSchedulerProcessPendingTasks(SchedulerCore& core,
                                           WakeCoordinator& wake_coordinator,
                                           ResumeFn&& resume_fn) {
    (void)core.runReadyPass(
        std::forward<ResumeFn>(resume_fn),
        [&](size_t drained) { wake_coordinator.onRemoteCollected(drained); });
}

/**
 * @brief IO 调度器主循环骨架：本地 follow-up、时间轮 tick、无待办时调用 poll_fn
 * @param post_passes_fn 在每次 runLocalFollowupPasses 之后调用（例如 kqueue 提交 m_pending_changes；
 *        当 hasPendingWork 为真时不会进入 poll，此处仍能保证延迟注册落地）
 */
template <typename ResumeFn, typename PollFn, typename PostPassesFn>
void runIOSchedulerEventLoop(std::atomic<bool>& running,
                             SchedulerCore& core,
                             TimingWheelTimerManager& timer_manager,
                             WakeCoordinator& wake_coordinator,
                             size_t batch_size,
                             ResumeFn&& resume_fn,
                             PollFn&& poll_fn,
                             PostPassesFn&& post_passes_fn) {
    const size_t bs = batch_size > 0 ? batch_size : 1;
    size_t local_followup_pass_limit = 4096 / bs;
    if (local_followup_pass_limit == 0) {
        local_followup_pass_limit = 1;
    }
    if (local_followup_pass_limit > 16) {
        local_followup_pass_limit = 16;
    }

    while (running.load(std::memory_order_acquire)) {
        (void)core.runLocalFollowupPasses(
            local_followup_pass_limit,
            std::forward<ResumeFn>(resume_fn),
            [&](size_t drained) { wake_coordinator.onRemoteCollected(drained); });
        post_passes_fn();
        timer_manager.tick();
        wake_coordinator.markSleeping();
        if (core.hasPendingWork()) {
            wake_coordinator.markAwake();
            continue;
        }
        if (core.trySteal()) {
            wake_coordinator.markAwake();
            continue;
        }
        poll_fn();
        wake_coordinator.markAwake();
    }
}

}  // namespace detail

}  // namespace galay::kernel

#endif
