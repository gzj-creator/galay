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

void EpollSchedulerBackend::pollBackend()
{
    m_reactor.poll(schedulerPollTimeoutMilliseconds(), m_wake_coordinator);
}

void EpollSchedulerBackend::flushBackend()
{
    // flush 将错误保存到 lastError；事件循环继续排空已接纳的任务。
    (void)m_reactor.flushPendingChanges();
}

} // namespace galay::kernel
#endif
