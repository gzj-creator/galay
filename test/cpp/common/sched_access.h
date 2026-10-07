#ifndef GALAY_TEST_SCHEDULER_TEST_ACCESS_H
#define GALAY_TEST_SCHEDULER_TEST_ACCESS_H

#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>

namespace galay::kernel {

struct SchedulerTestAccess {
    template <typename SchedulerT>
    static void process_pending(SchedulerT& scheduler) {
        scheduler.process_pending_tasks();
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
    static auto& wakeup_pending(SchedulerT& scheduler) {
        return scheduler.m_wakeup_pending;
    }

#ifdef USE_EPOLL
    static std::expected<void, IOError> start_reactor(EpollScheduler& scheduler) {
        return scheduler.m_reactor.start();
    }

    static int wake_read_fd(EpollScheduler& scheduler) {
        return scheduler.m_reactor.get_handle().fd;
    }

    static int flush_reactor(EpollScheduler& scheduler) {
        return scheduler.m_reactor.flush_pending_changes();
    }
#endif

#ifdef USE_IOURING
    static std::expected<void, IOError> start_reactor(IOUringScheduler& scheduler) {
        return scheduler.m_reactor.start();
    }

    static int wake_read_fd(IOUringScheduler& scheduler) {
        return scheduler.m_reactor.get_handle().fd;
    }
#endif

#ifdef USE_KQUEUE
    static std::expected<void, IOError> start_reactor(KqueueScheduler& scheduler) {
        return scheduler.m_reactor.start();
    }

    static int wake_read_fd(KqueueScheduler& scheduler) {
        return scheduler.m_reactor.get_handle().fd;
    }

    static int kqueue_fd(KqueueScheduler& scheduler) {
        return scheduler.m_reactor.get_handle().fd;
    }
#endif
};

}  // namespace galay::kernel

#endif
