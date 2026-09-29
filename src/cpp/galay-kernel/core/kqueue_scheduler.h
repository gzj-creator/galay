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

/**
 * @brief macOS/BSD kqueue 内置后端；公开操作遵守 IOSchedulerBase 的线程契约。
 * @tparam Config 调度器配置（编译期常量），默认使用标准配置
 */
template <typename Config = DefaultIOSchedulerConfig>
class KqueueSchedulerT : public IOSchedulerBase<KqueueSchedulerT<Config>, KqueueReactor>
{
public:
    using ConfigType = Config;

    /** @brief 使用编译期配置构造调度器 */
    explicit KqueueSchedulerT()
        : IOSchedulerBase<KqueueSchedulerT<Config>, KqueueReactor>(
            static_cast<int>(Config::kMaxEvents),
            static_cast<int>(Config::kBatchSize))
    {}

    /** @brief 在线程退出后再析构 reactor 和队列；调用前须关闭借用者的异步源。 */
    ~KqueueSchedulerT() { this->stop(); }

    KqueueSchedulerT(const KqueueSchedulerT&) = delete;
    KqueueSchedulerT& operator=(const KqueueSchedulerT&) = delete;

    /** @brief 获取编译期配置常量 */
    static constexpr size_t maxEvents() noexcept { return Config::kMaxEvents; }
    static constexpr size_t batchSize() noexcept { return Config::kBatchSize; }

private:
    friend class IOSchedulerBase<KqueueSchedulerT<Config>, KqueueReactor>;

    void pollBackend() {
        const uint64_t timeout_ns = this->schedulerPollTimeoutNanoseconds();
        this->m_reactor.poll(timeout_ns, this->m_wake_coordinator);
    }

    void flushBackend() {
        (void)this->m_reactor.flushPendingChanges();
    }
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
