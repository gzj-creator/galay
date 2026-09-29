/**
 * @file epoll_scheduler.h
 * @brief Linux epoll 调度器的构造与 poll 适配。
 * @details 生命周期、入队、唤醒和 IO 注册均复用 IOSchedulerBase。
 * 支持编译期配置模板以获得更好的性能优化。
 */
#ifndef GALAY_KERNEL_EPOLL_SCHEDULER_H
#define GALAY_KERNEL_EPOLL_SCHEDULER_H

#include "epoll_reactor.h"
#include "io_scheduler_base.hpp"
#include "../common/scheduler_config.h"

#ifdef USE_EPOLL
namespace galay::kernel {

/**
 * @brief Linux epoll 内置后端；公开操作遵守 IOSchedulerBase 的线程契约。
 * @tparam Config 调度器配置（编译期常量），默认使用标准配置
 */
template <typename Config = DefaultIOSchedulerConfig>
class EpollSchedulerT : public IOSchedulerBase<EpollSchedulerT<Config>, EpollReactor>
{
public:
    using ConfigType = Config;

    /** @brief 使用编译期配置构造调度器 */
    explicit EpollSchedulerT()
        : IOSchedulerBase<EpollSchedulerT<Config>, EpollReactor>(
            static_cast<int>(Config::kMaxEvents),
            static_cast<int>(Config::kBatchSize))
    {}

    /** @brief 在线程退出后再析构 reactor 和队列；调用前须关闭借用者的异步源。 */
    ~EpollSchedulerT() { this->stop(); }

    /** @brief Accept operation 的 owner 注册及 timeout 适配入口。 */
    bool submitAccept(AcceptAwaitable& awaitable, Waker&& waker) {
        return this->m_reactor.submitAccept(awaitable, std::move(waker));
    }

    void timeoutAccept(AcceptAwaitable& awaitable) {
        this->m_reactor.timeoutAccept(awaitable);
    }

    EpollSchedulerT(const EpollSchedulerT&) = delete;
    EpollSchedulerT& operator=(const EpollSchedulerT&) = delete;

    /** @brief 获取编译期配置常量 */
    static constexpr size_t maxEvents() noexcept { return Config::kMaxEvents; }
    static constexpr size_t batchSize() noexcept { return Config::kBatchSize; }

private:
    friend class IOSchedulerBase<EpollSchedulerT<Config>, EpollReactor>;

    void pollBackend() {
        const int timeout_ms = this->schedulerPollTimeoutMilliseconds();
        this->m_reactor.poll(timeout_ms, this->m_wake_coordinator);
    }

    void flushBackend() {
        (void)this->m_reactor.flushPendingChanges();
    }
};

/** @brief 默认 epoll 调度器 */
using EpollScheduler = EpollSchedulerT<DefaultIOSchedulerConfig>;

/** @brief 高性能 epoll 调度器 */
using HighPerformanceEpollScheduler = EpollSchedulerT<HighPerformanceIOSchedulerConfig>;

/** @brief 低延迟 epoll 调度器 */
using LowLatencyEpollScheduler = EpollSchedulerT<LowLatencyIOSchedulerConfig>;

} // namespace galay::kernel
#endif
#endif
