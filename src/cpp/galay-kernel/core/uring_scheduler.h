/**
 * @file uring_scheduler.h
 * @brief Linux io_uring 调度器的构造与 poll 适配。
 * @details 生命周期、入队、唤醒和 IO 注册均复用 IOSchedulerBase。
 * 支持编译期配置模板以获得更好的性能优化。
 */
#ifndef GALAY_KERNEL_URING_SCHEDULER_H
#define GALAY_KERNEL_URING_SCHEDULER_H

#include "uring_reactor.h"
#include "io_scheduler_base.hpp"
#include "../common/scheduler_config.h"

#ifdef USE_IOURING
namespace galay::kernel {

/**
 * @brief Linux io_uring 内置后端；公开操作遵守 IOSchedulerBase 的线程契约。
 * @tparam Config 调度器配置（编译期常量），默认使用标准配置
 */
template <typename Config = DefaultIOSchedulerConfig>
class IOUringSchedulerT : public IOSchedulerBase<IOUringSchedulerT<Config>, IOUringReactor>
{
public:
    using ConfigType = Config;

    /** @brief 使用编译期配置构造调度器 */
    explicit IOUringSchedulerT()
        : IOSchedulerBase<IOUringSchedulerT<Config>, IOUringReactor>(
            static_cast<int>(Config::kQueueDepth),
            static_cast<int>(Config::kBatchSize))
    {}

    /** @brief 在线程退出后再析构 reactor 和队列；调用前须关闭借用者的异步源。 */
    ~IOUringSchedulerT() { this->stop(); }

    /** @brief 单次 accept 的 owner 注册/timeout 入口；不等待持久 SQE 终止。 */
    bool submitAccept(AcceptAwaitable& awaitable, Waker&& waker) {
        return this->m_reactor.submitAccept(awaitable, std::move(waker));
    }

    void timeoutAccept(AcceptAwaitable& awaitable) {
        this->m_reactor.timeoutAccept(awaitable);
    }

    IOUringSchedulerT(const IOUringSchedulerT&) = delete;
    IOUringSchedulerT& operator=(const IOUringSchedulerT&) = delete;

    /** @brief 获取编译期配置常量 */
    static constexpr size_t queueDepth() noexcept { return Config::kQueueDepth; }
    static constexpr size_t batchSize() noexcept { return Config::kBatchSize; }

private:
    friend class IOSchedulerBase<IOUringSchedulerT<Config>, IOUringReactor>;

    void pollBackend() {
        const uint64_t timeout_ns = this->schedulerPollTimeoutNanoseconds();
        this->m_reactor.poll(timeout_ns, this->m_wake_coordinator);
    }

    void flushBackend() {
        this->m_reactor.flush();
    }
};

/** @brief 默认 io_uring 调度器 */
using IOUringScheduler = IOUringSchedulerT<DefaultIOSchedulerConfig>;

/** @brief 高性能 io_uring 调度器 */
using HighPerformanceIOUringScheduler = IOUringSchedulerT<HighPerformanceIOSchedulerConfig>;

/** @brief 低延迟 io_uring 调度器 */
using LowLatencyIOUringScheduler = IOUringSchedulerT<LowLatencyIOSchedulerConfig>;

} // namespace galay::kernel
#endif
#endif
