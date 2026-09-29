#include "uring_scheduler.h"

#ifdef USE_IOURING
namespace galay::kernel {

IOUringSchedulerBackend::IOUringSchedulerBackend(int queue_depth, int batch_size)
    : IOSchedulerBase<IOUringSchedulerBackend, IOUringReactor>(queue_depth, batch_size)
{
}

IOUringSchedulerBackend::~IOUringSchedulerBackend()
{
    stop();
}

void IOUringSchedulerBackend::pollBackend()
{
    m_reactor.poll(schedulerPollTimeoutIoUringNanoseconds(), m_wake_coordinator);
}

void IOUringSchedulerBackend::flushBackend()
{
    // io_uring 请求由 reactor 提交，不需要 readiness 后端的延后注册 flush。
}

} // namespace galay::kernel
#endif
