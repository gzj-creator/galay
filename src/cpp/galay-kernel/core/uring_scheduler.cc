#include "uring_scheduler.h"

#ifdef USE_IOURING
namespace galay::kernel {

IOUringScheduler::IOUringScheduler(int queue_depth, int batch_size)
    : IOSchedulerBase<IOUringScheduler, IOUringReactor>(queue_depth, batch_size)
{
}

IOUringScheduler::~IOUringScheduler()
{
    stop();
}

void IOUringScheduler::pollBackend()
{
    m_reactor.poll(schedulerPollTimeoutIoUringNanoseconds(), m_wake_coordinator);
}

void IOUringScheduler::flushBackend()
{
    // io_uring 请求由 reactor 提交，不需要 readiness 后端的延后注册 flush。
}

} // namespace galay::kernel
#endif
