/**
 * @file kqueue_scheduler_v2.h
 * @brief 模板化的 macOS kqueue 调度器
 * @details 使用编译期配置模板以获得更好的性能优化
 */
#ifndef GALAY_KERNEL_KQUEUE_SCHEDULER_V2_H
#define GALAY_KERNEL_KQUEUE_SCHEDULER_V2_H

#include "kqueue_reactor.h"
#include "io_scheduler_base.hpp"
#include "../common/scheduler_config.h"

#ifdef USE_KQUEUE
namespace galay::kernel {

/**
 * @brief 模板化的 macOS kqueue 调度器
 * @tparam Config 调度器配置（编译期常量）
 */
template <typename Config = DefaultIOSchedulerConfig>
class KqueueSchedulerV2 : public IOSchedulerBase<KqueueSchedulerV2<Config>, KqueueReactor>
{
public:
    using ConfigType = Config;

    /** @brief 使用编译期配置构造调度器 */
    explicit KqueueSchedulerV2()
        : IOSchedulerBase<KqueueSchedulerV2<Config>, KqueueReactor>(
            static_cast<int>(Config::kMaxEvents),
            static_cast<int>(Config::kBatchSize))
    {
    }

    /** @brief 析构前自动停止 */
    ~KqueueSchedulerV2() { this->stop(); }

    KqueueSchedulerV2(const KqueueSchedulerV2&) = delete;
    KqueueSchedulerV2& operator=(const KqueueSchedulerV2&) = delete;

    /** @brief 获取编译期配置常量 */
    static constexpr size_t maxEvents() noexcept { return Config::kMaxEvents; }
    static constexpr size_t batchSize() noexcept { return Config::kBatchSize; }

private:
    friend class IOSchedulerBase<KqueueSchedulerV2<Config>, KqueueReactor>;

    void pollBackend() {
        const uint64_t timeout_ns = this->schedulerPollTimeoutNanoseconds();
        this->m_reactor.poll(timeout_ns, this->m_wake_coordinator);
    }

    void flushBackend() {
        (void)this->m_reactor.flushPendingChanges();
    }
};

/**
 * @brief 默认 kqueue 调度器（向后兼容别名）
 */
using DefaultKqueueScheduler = KqueueSchedulerV2<DefaultIOSchedulerConfig>;

/**
 * @brief 高性能 kqueue 调度器
 */
using HighPerformanceKqueueScheduler = KqueueSchedulerV2<HighPerformanceIOSchedulerConfig>;

/**
 * @brief 低延迟 kqueue 调度器
 */
using LowLatencyKqueueScheduler = KqueueSchedulerV2<LowLatencyIOSchedulerConfig>;

} // namespace galay::kernel
#endif // USE_KQUEUE
#endif // GALAY_KERNEL_KQUEUE_SCHEDULER_V2_H
