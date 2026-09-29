#include "kqueue_scheduler.h"

#ifdef USE_KQUEUE
namespace galay::kernel {

// 显式实例化默认配置的调度器
template class KqueueSchedulerT<DefaultIOSchedulerConfig>;

// 显式实例化高性能配置的调度器
template class KqueueSchedulerT<HighPerformanceIOSchedulerConfig>;

// 显式实例化低延迟配置的调度器
template class KqueueSchedulerT<LowLatencyIOSchedulerConfig>;

} // namespace galay::kernel
#endif
