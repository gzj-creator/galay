/**
 * @file kqueue_scheduler.h
 * @brief macOS/BSD kqueue 调度器的构造与 poll 适配。
 * @details 生命周期、入队、唤醒和 IO 注册均复用 IOSchedulerBase。
 */
#ifndef GALAY_KERNEL_KQUEUE_SCHEDULER_H
#define GALAY_KERNEL_KQUEUE_SCHEDULER_H

#include "kqueue_reactor.h"
#include "io_scheduler_base.hpp"

#ifdef USE_KQUEUE
namespace galay::kernel {

/** @brief macOS/BSD kqueue 内置后端；公开操作遵守 IOSchedulerBase 的线程契约。 */
class KqueueScheduler : public IOSchedulerBase<KqueueScheduler, KqueueReactor>
{
public:
    /** @param max_events 后端单次事件容量；batch_size 为就绪任务批处理预算。 */
    explicit KqueueScheduler(int max_events = GALAY_SCHEDULER_MAX_EVENTS,
                          int batch_size = GALAY_SCHEDULER_BATCH_SIZE);
    /** @brief 在线程退出后再析构 reactor 和队列；调用前须关闭借用者的异步源。 */
    ~KqueueScheduler();

    KqueueScheduler(const KqueueScheduler&) = delete;
    KqueueScheduler& operator=(const KqueueScheduler&) = delete;

private:
    friend class IOSchedulerBase<KqueueScheduler, KqueueReactor>;
    void pollBackend();
    void flushBackend();
};

} // namespace galay::kernel
#endif
#endif
