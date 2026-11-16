#ifndef GALAY_TEST_SCHEDULER_TEST_ACCESS_H
#define GALAY_TEST_SCHEDULER_TEST_ACCESS_H

#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>

namespace galay::kernel {

struct SchedulerTestAccess {
#ifdef USE_EPOLL
    template <typename Config>
    static auto& io_base(EpollSchedulerT<Config>& scheduler) {
        return static_cast<IOSchedulerBase<EpollSchedulerBackend, EpollReactor>&>(scheduler);
    }
#endif

#ifdef USE_IOURING
    template <typename Config>
    static auto& io_base(IOUringSchedulerT<Config>& scheduler) {
        return static_cast<IOSchedulerBase<IOUringSchedulerBackend, IOUringReactor>&>(scheduler);
    }
#endif

#ifdef USE_KQUEUE
    template <typename Config>
    static auto& io_base(KqueueSchedulerT<Config>& scheduler) {
        return static_cast<IOSchedulerBase<KqueueSchedulerBackend, KqueueReactor>&>(scheduler);
    }
#endif

    template <typename SchedulerT>
    static size_t process_pending(SchedulerT& scheduler) {
        auto& base = io_base(scheduler);
        return base.m_core.run_ready_pass(
            [&base](TaskRef& task) { base.resume(task); },
            [&base](size_t drained) {
                base.m_wake_coordinator.on_remote_collected(drained);
            });
    }

    template <typename SchedulerT>
    static auto& worker(SchedulerT& scheduler) {
        return io_base(scheduler).m_worker;
    }

    template <typename SchedulerT>
    static auto& sleeping(SchedulerT& scheduler) {
        return io_base(scheduler).m_sleeping;
    }

    template <typename SchedulerT>
    static auto& wakeup_pending(SchedulerT& scheduler) {
        return io_base(scheduler).m_wakeup_pending;
    }

#ifdef USE_EPOLL
    static std::expected<void, IOError> start_reactor(EpollScheduler& scheduler) {
        return io_base(scheduler).m_reactor.start();
    }

    static int wake_read_fd(EpollScheduler& scheduler) {
        return io_base(scheduler).m_reactor.get_handle().fd;
    }

    static int flush_reactor(EpollScheduler& scheduler) {
        return io_base(scheduler).m_reactor.flush_pending_changes();
    }
#endif

#ifdef USE_IOURING
    static std::expected<void, IOError> start_reactor(IOUringScheduler& scheduler) {
        return io_base(scheduler).m_reactor.start();
    }

    static int wake_read_fd(IOUringScheduler& scheduler) {
        return io_base(scheduler).m_reactor.get_handle().fd;
    }
#endif

#ifdef USE_KQUEUE
    static std::expected<void, IOError> start_reactor(KqueueScheduler& scheduler) {
        return io_base(scheduler).m_reactor.start();
    }

    static int wake_read_fd(KqueueScheduler& scheduler) {
        return io_base(scheduler).m_reactor.get_handle().fd;
    }

    static int kqueue_fd(KqueueScheduler& scheduler) {
        return io_base(scheduler).m_reactor.get_handle().fd;
    }
#endif
};

}  // namespace galay::kernel

#endif
