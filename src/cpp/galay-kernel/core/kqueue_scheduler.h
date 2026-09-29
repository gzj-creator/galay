/**
 * @file kqueue_scheduler.h
 * @brief macOS/BSD kqueue 调度器的构造与 poll 适配。
 * @details 生命周期、入队、唤醒和 IO 注册均复用 IOSchedulerBase。
 * 支持编译期配置模板以获得更好的性能优化。
 */
#ifndef GALAY_KERNEL_KQUEUE_SCHEDULER_H
#define GALAY_KERNEL_KQUEUE_SCHEDULER_H

#include "kqueue_reactor.h"
#include "io_scheduler_base.hpp"
#include "../common/scheduler_config.h"

#ifdef USE_KQUEUE
namespace galay::kernel {

/** @brief 同一后端所有配置共享的调度与 IO 注册实现；仅允许派生配置类型构造。 */
class KqueueSchedulerBackend : public IOSchedulerBase<KqueueSchedulerBackend, KqueueReactor>
{
protected:
    explicit KqueueSchedulerBackend(int max_events, int batch_size);
    /** @brief 在线程退出后再析构 reactor 和队列；调用前须关闭借用者的异步源。 */
    ~KqueueSchedulerBackend();

private:
    friend class IOSchedulerBase<KqueueSchedulerBackend, KqueueReactor>;
    void pollBackend();
    void flushBackend();
};

/** @brief 配置模板只负责构造容量；公共借用入口始终分派到同一真实后端基类。 */
template <typename Config = DefaultIOSchedulerConfig>
class KqueueSchedulerT : public KqueueSchedulerBackend
{
public:
    using ConfigType = Config;

    explicit KqueueSchedulerT()
        : KqueueSchedulerBackend(
            static_cast<int>(Config::kMaxEvents), static_cast<int>(Config::kBatchSize))
    {}

    KqueueSchedulerT(const KqueueSchedulerT&) = delete;
    KqueueSchedulerT& operator=(const KqueueSchedulerT&) = delete;

    static constexpr size_t maxEvents() noexcept { return Config::kMaxEvents; }
    static constexpr size_t batchSize() noexcept { return Config::kBatchSize; }
};

/** @brief 默认 kqueue 调度器 */
using KqueueScheduler = KqueueSchedulerT<DefaultIOSchedulerConfig>;

/** @brief 高性能 kqueue 调度器 */
using HighPerformanceKqueueScheduler = KqueueSchedulerT<HighPerformanceIOSchedulerConfig>;

/** @brief 低延迟 kqueue 调度器 */
using LowLatencyKqueueScheduler = KqueueSchedulerT<LowLatencyIOSchedulerConfig>;

} // namespace galay::kernel
#endif
#endif
