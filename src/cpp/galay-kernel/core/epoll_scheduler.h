/**
 * @file epoll_scheduler.h
 * @brief Linux epoll 调度器的构造与 poll 适配。
 * @details 生命周期、入队、唤醒和 IO 注册均复用 IOSchedulerBase。
 */
#ifndef GALAY_KERNEL_EPOLL_SCHEDULER_H
#define GALAY_KERNEL_EPOLL_SCHEDULER_H

#include "epoll_reactor.h"
#include "io_scheduler_base.hpp"

#ifdef USE_EPOLL
namespace galay::kernel {

/** @brief Linux epoll 内置后端；公开操作遵守 IOSchedulerBase 的线程契约。 */
class EpollScheduler : public IOSchedulerBase<EpollScheduler, EpollReactor>
{
public:
    /** @param max_events 后端单次事件容量；batch_size 为就绪任务批处理预算。 */
    explicit EpollScheduler(int max_events = GALAY_SCHEDULER_MAX_EVENTS,
                          int batch_size = GALAY_SCHEDULER_BATCH_SIZE);
    /** @brief 在线程退出后再析构 reactor 和队列；调用前须关闭借用者的异步源。 */
    ~EpollScheduler();
    /** @brief Accept operation 的 owner 注册及 timeout 适配入口。 */
    bool submitAccept(AcceptAwaitable& awaitable, Waker&& waker) {
        return m_reactor.submitAccept(awaitable, std::move(waker));
    }
    void timeoutAccept(AcceptAwaitable& awaitable) { m_reactor.timeoutAccept(awaitable); }

    EpollScheduler(const EpollScheduler&) = delete;
    EpollScheduler& operator=(const EpollScheduler&) = delete;

private:
    friend class IOSchedulerBase<EpollScheduler, EpollReactor>;
    void pollBackend();
    void flushBackend();
};

} // namespace galay::kernel
#endif
#endif
