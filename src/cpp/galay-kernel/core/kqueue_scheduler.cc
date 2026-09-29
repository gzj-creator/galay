#include "kqueue_scheduler.h"

#ifdef USE_KQUEUE
namespace galay::kernel {

KqueueSchedulerBackend::KqueueSchedulerBackend(int max_events, int batch_size)
    : IOSchedulerBase<KqueueSchedulerBackend, KqueueReactor>(max_events, batch_size)
{
}

KqueueSchedulerBackend::~KqueueSchedulerBackend()
{
    stop();
}

void KqueueSchedulerBackend::pollBackend()
{
    const uint64_t ns = schedulerPollTimeoutNanoseconds();
    const timespec timeout{
        .tv_sec = static_cast<::time_t>(ns / 1'000'000'000ULL),
        .tv_nsec = static_cast<long>(ns % 1'000'000'000ULL),
    };
    m_reactor.poll(timeout, m_wake_coordinator);
}

void KqueueSchedulerBackend::flushBackend()
{
    // flush 将错误保存到 lastError；事件循环继续排空已接纳的任务。
    (void)m_reactor.flushPendingChanges();
}

} // namespace galay::kernel
#endif
