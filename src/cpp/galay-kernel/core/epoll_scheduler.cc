#include "epoll_scheduler.h"

#ifdef USE_EPOLL
namespace galay::kernel {

EpollSchedulerBackend::EpollSchedulerBackend(int max_events, int batch_size)
    : IOSchedulerBase<EpollSchedulerBackend, EpollReactor>(max_events, batch_size)
{
}

EpollSchedulerBackend::~EpollSchedulerBackend()
{
    stop();
}

void EpollSchedulerBackend::poll_backend()
{
    m_reactor.poll(scheduler_poll_timeout_milliseconds(), m_wake_coordinator);
}

void EpollSchedulerBackend::flush_backend()
{
    // flush 将错误保存到 last_error；事件循环继续排空已接纳的任务。
    (void)m_reactor.flush_pending_changes();
}

} // namespace galay::kernel
#endif
