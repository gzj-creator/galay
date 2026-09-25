#include "epoll_scheduler.h"

#ifdef USE_EPOLL
namespace galay::kernel {

EpollScheduler::EpollScheduler(int max_events, int batch_size)
    : IOSchedulerBase<EpollScheduler, EpollReactor>(max_events, batch_size)
{
}

EpollScheduler::~EpollScheduler()
{
    stop();
}

void EpollScheduler::pollBackend()
{
    m_reactor.poll(schedulerPollTimeoutMilliseconds(), m_wake_coordinator);
}

void EpollScheduler::flushBackend()
{
    // flush 将错误保存到 lastError；事件循环继续排空已接纳的任务。
    (void)m_reactor.flushPendingChanges();
}

} // namespace galay::kernel
#endif
