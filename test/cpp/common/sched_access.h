#ifndef GALAY_TEST_SCHEDULER_TEST_ACCESS_H
#define GALAY_TEST_SCHEDULER_TEST_ACCESS_H

#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>

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
    static std::expected<void, IOError> startReactor(EpollScheduler& scheduler) {
        return scheduler.m_reactor.start();
    }

    static int wakeReadFd(EpollScheduler& scheduler) {
        return scheduler.m_reactor.getHandle().fd;
    }
#endif

#ifdef USE_IOURING
    static std::expected<void, IOError> startReactor(IOUringScheduler& scheduler) {
        return scheduler.m_reactor.start();
    }

    static int wakeReadFd(IOUringScheduler& scheduler) {
        return scheduler.m_reactor.getHandle().fd;
    }
#endif

#ifdef USE_KQUEUE
    static std::expected<void, IOError> startReactor(KqueueScheduler& scheduler) {
        return scheduler.m_reactor.start();
    }

    static int wakeReadFd(KqueueScheduler& scheduler) {
        return scheduler.m_reactor.getHandle().fd;
    }
#endif
};

}  // namespace galay::kernel

#endif
