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

void KqueueSchedulerBackend::poll_backend()
{
    const uint64_t ns = scheduler_poll_timeout_nanoseconds();
    const timespec timeout{
        .tv_sec = static_cast<::time_t>(ns / 1'000'000'000ULL),
        .tv_nsec = static_cast<long>(ns % 1'000'000'000ULL),
    };
    m_reactor.poll(timeout, m_wake_coordinator);
}

void KqueueSchedulerBackend::flush_backend()
{
    // flush 将错误保存到 last_error；事件循环继续排空已接纳的任务。
    (void)m_reactor.flush_pending_changes();
}

} // namespace galay::kernel
#endif
