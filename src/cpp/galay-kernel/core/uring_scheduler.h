/**
 * @file uring_scheduler.h
 * @brief Linux io_uring 调度器的构造与 poll 适配。
 * @details 生命周期、入队、唤醒和 IO 注册均复用 IOSchedulerBase。
 */
#ifndef GALAY_KERNEL_URING_SCHEDULER_H
#define GALAY_KERNEL_URING_SCHEDULER_H

#include "uring_reactor.h"
#include "io_scheduler_base.hpp"

#ifdef USE_IOURING
namespace galay::kernel {

/** @brief Linux io_uring 内置后端；公开操作遵守 IOSchedulerBase 的线程契约。 */
class IOUringScheduler : public IOSchedulerBase<IOUringScheduler, IOUringReactor>
{
public:
    /** @param queue_depth 后端单次事件容量；batch_size 为就绪任务批处理预算。 */
    explicit IOUringScheduler(int queue_depth = GALAY_SCHEDULER_QUEUE_DEPTH,
                          int batch_size = GALAY_SCHEDULER_BATCH_SIZE);
    /** @brief 在线程退出后再析构 reactor 和队列；调用前须关闭借用者的异步源。 */
    ~IOUringScheduler();
    /** @brief 单次 accept 的 owner 注册/timeout 入口；不等待持久 SQE 终止。 */
    bool submitAccept(AcceptAwaitable& awaitable, Waker&& waker) {
        return m_reactor.submitAccept(awaitable, std::move(waker));
    }
    void timeoutAccept(AcceptAwaitable& awaitable) { m_reactor.timeoutAccept(awaitable); }

    IOUringScheduler(const IOUringScheduler&) = delete;
    IOUringScheduler& operator=(const IOUringScheduler&) = delete;

private:
    friend class IOSchedulerBase<IOUringScheduler, IOUringReactor>;
    void pollBackend();
    void flushBackend();
};

} // namespace galay::kernel
#endif
#endif
