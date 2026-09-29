/**
 * @file epoll_scheduler_v2.h
 * @brief 模板化的 Linux epoll 调度器
 * @details 使用编译期配置模板以获得更好的性能优化
 */
#ifndef GALAY_KERNEL_EPOLL_SCHEDULER_V2_H
#define GALAY_KERNEL_EPOLL_SCHEDULER_V2_H

#include "epoll_reactor.h"
#include "io_scheduler_base.hpp"
#include "../common/scheduler_config.h"

#ifdef USE_EPOLL
namespace galay::kernel {

/**
 * @brief 模板化的 Linux epoll 调度器
 * @tparam Config 调度器配置（编译期常量）
 */
template <typename Config = DefaultIOSchedulerConfig>
class EpollSchedulerV2 : public IOSchedulerBase<EpollSchedulerV2<Config>, EpollReactor>
{
public:
    using ConfigType = Config;

    /** @brief 使用编译期配置构造调度器 */
    explicit EpollSchedulerV2()
        : IOSchedulerBase<EpollSchedulerV2<Config>, EpollReactor>(
            static_cast<int>(Config::kMaxEvents),
            static_cast<int>(Config::kBatchSize))
    {
    }

    /** @brief 析构前自动停止 */
    ~EpollSchedulerV2() { this->stop(); }

    /** @brief Accept operation 的 owner 注册及 timeout 适配入口 */
    bool submitAccept(AcceptAwaitable& awaitable, Waker&& waker) {
        return this->m_reactor.submitAccept(awaitable, std::move(waker));
    }

    void timeoutAccept(AcceptAwaitable& awaitable) {
        this->m_reactor.timeoutAccept(awaitable);
    }

    EpollSchedulerV2(const EpollSchedulerV2&) = delete;
    EpollSchedulerV2& operator=(const EpollSchedulerV2&) = delete;

    /** @brief 获取编译期配置常量 */
    static constexpr size_t maxEvents() noexcept { return Config::kMaxEvents; }
    static constexpr size_t batchSize() noexcept { return Config::kBatchSize; }

private:
    friend class IOSchedulerBase<EpollSchedulerV2<Config>, EpollReactor>;

    void pollBackend() {
        const int timeout_ms = this->schedulerPollTimeoutMilliseconds();
        this->m_reactor.poll(timeout_ms, this->m_wake_coordinator);
    }

    void flushBackend() {
        (void)this->m_reactor.flushPendingChanges();
    }
};

/**
 * @brief 默认 epoll 调度器（向后兼容别名）
 */
using DefaultEpollScheduler = EpollSchedulerV2<DefaultIOSchedulerConfig>;

/**
 * @brief 高性能 epoll 调度器
 */
using HighPerformanceEpollScheduler = EpollSchedulerV2<HighPerformanceIOSchedulerConfig>;

/**
 * @brief 低延迟 epoll 调度器
 */
using LowLatencyEpollScheduler = EpollSchedulerV2<LowLatencyIOSchedulerConfig>;

} // namespace galay::kernel
#endif // USE_EPOLL
#endif // GALAY_KERNEL_EPOLL_SCHEDULER_V2_H
