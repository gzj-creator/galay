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

/** @brief 同一后端所有配置共享的调度与 IO 注册实现；仅允许派生配置类型构造。 */
class IOUringSchedulerBackend : public IOSchedulerBase<IOUringSchedulerBackend, IOUringReactor>
{
public:
    /** @brief Accept operation 的 owner 注册及 timeout 适配入口。 */
    bool submitAccept(AcceptAwaitable& awaitable, Waker&& waker) {
        return m_reactor.submitAccept(awaitable, std::move(waker));
    }
    void timeoutAccept(AcceptAwaitable& awaitable) { m_reactor.timeoutAccept(awaitable); }

protected:
    explicit IOUringSchedulerBackend(int queue_depth, int batch_size);
    /** @brief 在线程退出后再析构 reactor 和队列；调用前须关闭借用者的异步源。 */
    ~IOUringSchedulerBackend();

private:
    friend class IOSchedulerBase<IOUringSchedulerBackend, IOUringReactor>;
    void pollBackend();
    void flushBackend();
};

/** @brief 配置模板只负责构造容量；公共借用入口始终分派到同一真实后端基类。 */
template <typename Config = DefaultIOSchedulerConfig>
class IOUringSchedulerT : public IOUringSchedulerBackend
{
public:
    using ConfigType = Config;

    explicit IOUringSchedulerT()
        : IOUringSchedulerBackend(
            static_cast<int>(Config::kQueueDepth), static_cast<int>(Config::kBatchSize))
    {}

    IOUringSchedulerT(const IOUringSchedulerT&) = delete;
    IOUringSchedulerT& operator=(const IOUringSchedulerT&) = delete;

    static constexpr size_t queueDepth() noexcept { return Config::kQueueDepth; }
    static constexpr size_t batchSize() noexcept { return Config::kBatchSize; }
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
