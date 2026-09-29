#include "uring_scheduler.h"

#ifdef USE_IOURING
namespace galay::kernel {

// 显式实例化默认配置的调度器
template class IOUringSchedulerT<DefaultIOSchedulerConfig>;

// 显式实例化高性能配置的调度器
template class IOUringSchedulerT<HighPerformanceIOSchedulerConfig>;

// 显式实例化低延迟配置的调度器
template class IOUringSchedulerT<LowLatencyIOSchedulerConfig>;

} // namespace galay::kernel
#endif
