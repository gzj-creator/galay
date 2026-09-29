/**
 * @file uring_scheduler_v2.h
 * @brief 模板化的 Linux io_uring 调度器
 * @details 使用编译期配置模板以获得更好的性能优化
 */
#ifndef GALAY_KERNEL_URING_SCHEDULER_V2_H
#define GALAY_KERNEL_URING_SCHEDULER_V2_H

#include "uring_reactor.h"
#include "io_scheduler_base.hpp"
#include "../common/scheduler_config.h"

#ifdef USE_IOURING
namespace galay::kernel {

/**
 * @brief 模板化的 Linux io_uring 调度器
 * @tparam Config 调度器配置（编译期常量）
 */
template <typename Config = DefaultIOSchedulerConfig>
class IOUringSchedulerV2 : public IOSchedulerBase<IOUringSchedulerV2<Config>, IOUringReactor>
{
public:
    using ConfigType = Config;

    /** @brief 使用编译期配置构造调度器 */
    explicit IOUringSchedulerV2()
        : IOSchedulerBase<IOUringSchedulerV2<Config>, IOUringReactor>(
            static_cast<int>(Config::kQueueDepth),
            static_cast<int>(Config::kBatchSize))
    {
    }

    /** @brief 析构前自动停止 */
    ~IOUringSchedulerV2() { this->stop(); }

    IOUringSchedulerV2(const IOUringSchedulerV2&) = delete;
    IOUringSchedulerV2& operator=(const IOUringSchedulerV2&) = delete;

    /** @brief 获取编译期配置常量 */
    static constexpr size_t queueDepth() noexcept { return Config::kQueueDepth; }
    static constexpr size_t batchSize() noexcept { return Config::kBatchSize; }

private:
    friend class IOSchedulerBase<IOUringSchedulerV2<Config>, IOUringReactor>;

    void pollBackend() {
        const uint64_t timeout_ns = this->schedulerPollTimeoutNanoseconds();
        this->m_reactor.poll(timeout_ns, this->m_wake_coordinator);
    }

    void flushBackend() {
        this->m_reactor.flush();
    }
};

/**
 * @brief 默认 io_uring 调度器（向后兼容别名）
 */
using DefaultIOUringScheduler = IOUringSchedulerV2<DefaultIOSchedulerConfig>;

/**
 * @brief 高性能 io_uring 调度器
 */
using HighPerformanceIOUringScheduler = IOUringSchedulerV2<HighPerformanceIOSchedulerConfig>;

/**
 * @brief 低延迟 io_uring 调度器
 */
using LowLatencyIOUringScheduler = IOUringSchedulerV2<LowLatencyIOSchedulerConfig>;

} // namespace galay::kernel
#endif // USE_IOURING
#endif // GALAY_KERNEL_URING_SCHEDULER_V2_H
