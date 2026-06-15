#ifndef GALAY_TEST_SCHEDULER_TEST_ACCESS_H
#define GALAY_TEST_SCHEDULER_TEST_ACCESS_H

#include "galay-kernel/core/kqueue_scheduler.h"
#include "galay-kernel/core/epoll_scheduler.h"
#include "galay-kernel/core/uring_scheduler.h"

namespace galay::kernel {

struct SchedulerTestAccess {
    template <typename SchedulerT>
    static void processPending(SchedulerT& scheduler) {
        scheduler.processPendingTasks();
    }

    template <typename SchedulerT>
    static auto& worker(SchedulerT& scheduler) {
        return scheduler.m_worker;
    }

    template <typename SchedulerT>
    static auto& sleeping(SchedulerT& scheduler) {
        return scheduler.m_sleeping;
    }

    template <typename SchedulerT>
    static auto& wakeupPending(SchedulerT& scheduler) {
        return scheduler.m_wakeup_pending;
    }

#ifdef USE_EPOLL
    static int wakeReadFd(EpollScheduler& scheduler) {
        return scheduler.m_reactor.wakeReadFdForTest();
    }
#endif

#ifdef USE_IOURING
    static int wakeReadFd(IOUringScheduler& scheduler) {
        return scheduler.m_reactor.wakeReadFdForTest();
    }
#endif

#ifdef USE_KQUEUE
    static int wakeReadFd(KqueueScheduler& scheduler) {
        return scheduler.m_reactor.wakeReadFdForTest();
    }
#endif
};

}  // namespace galay::kernel

#endif
