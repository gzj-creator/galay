/** @brief 编译期选择的 IO 调度器；调用方无需传播模板参数。 */
#ifndef GALAY_KERNEL_IOSCHEDULER_HPP
#define GALAY_KERNEL_IOSCHEDULER_HPP

#if defined(USE_IOURING)
#include "uring_scheduler.h"
#elif defined(USE_KQUEUE)
#include "kqueue_scheduler.h"
#elif defined(USE_EPOLL)
#include "epoll_scheduler.h"
#else
#error "An IO scheduler backend must be selected"
#endif

#endif
