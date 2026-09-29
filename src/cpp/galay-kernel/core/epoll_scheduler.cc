#include "epoll_scheduler.h"

#ifdef USE_EPOLL
namespace galay::kernel {

// 显式实例化默认配置的调度器
template class EpollSchedulerT<DefaultIOSchedulerConfig>;

// 显式实例化高性能配置的调度器
template class EpollSchedulerT<HighPerformanceIOSchedulerConfig>;

// 显式实例化低延迟配置的调度器
template class EpollSchedulerT<LowLatencyIOSchedulerConfig>;

} // namespace galay::kernel
#endif
