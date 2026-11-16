/**
 * @file epoll_scheduler.h
 * @brief Linux epoll 调度器的构造与 poll 适配。
 * @details 生命周期、入队、唤醒和 IO 注册均复用 IOSchedulerBase。
 * 支持编译期容量选择和范围检查，事件循环使用共享后端实现。
 */
#ifndef GALAY_KERNEL_EPOLL_SCHEDULER_H
#define GALAY_KERNEL_EPOLL_SCHEDULER_H

#include "epoll_reactor.h"
#include "io_scheduler_adapter.hpp"

#ifdef USE_EPOLL
namespace galay::kernel {

/** @brief 同一后端所有配置共享的调度与 IO 注册实现；仅允许派生配置类型构造。 */
class EpollSchedulerBackend
    : public detail::IOSchedulerAdapter<
          EpollSchedulerBackend,
          EpollReactor,
          detail::IOSchedulerBackendKind::kEpoll>
{
public:
    /**
     * @brief Accept operation 的 owner 注册及 timeout 适配入口。
     * @param awaitable 等待体
     * @param waker 协程唤醒器
     * @return accept 操作注册成功时返回 true，否则返回 false
     */
    bool submit_accept(AcceptAwaitable& awaitable, Waker&& waker) {
        return m_reactor.submit_accept(awaitable, std::move(waker));
    }
    void timeout_accept(AcceptAwaitable& awaitable) { m_reactor.timeout_accept(awaitable); }

protected:
    using Adapter = detail::IOSchedulerAdapter<
        EpollSchedulerBackend,
        EpollReactor,
        detail::IOSchedulerBackendKind::kEpoll>;
    using Adapter::Adapter;
    /** @brief 在线程退出后再析构 reactor 和队列；调用前须关闭借用者的异步源。 */
    ~EpollSchedulerBackend() = default;
};

/** @brief 配置模板只负责构造容量；公共借用入口始终分派到同一真实后端基类。 */
template <typename Config = DefaultIOSchedulerConfig>
class EpollSchedulerT
    : public detail::IOSchedulerMaxEventsAdapter<EpollSchedulerBackend, Config>
{
};

/** @brief 默认 epoll 调度器 */
using EpollScheduler = EpollSchedulerT<DefaultIOSchedulerConfig>;

} // namespace galay::kernel
#endif
#endif
