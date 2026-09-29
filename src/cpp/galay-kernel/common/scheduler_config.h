/**
 * @file scheduler_config.h
 * @brief 调度器配置模板系统
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供编译期配置模板，替代运行时配置参数以获得更好的性能。
 * 通过模板参数化配置，编译器可以进行常量传播、循环展开等优化。
 */

#ifndef GALAY_KERNEL_SCHEDULER_CONFIG_H
#define GALAY_KERNEL_SCHEDULER_CONFIG_H

#include "kernel_config.h"
#include <cstddef>

namespace galay::kernel {

/**
 * @brief IO 调度器配置模板
 * @tparam MaxEvents 单次 poll 处理的最大事件数
 * @tparam BatchSize 单批恢复的协程批量大小
 * @tparam QueueDepth io_uring 提交/完成队列深度（仅用于 io_uring）
 */
template <
    size_t MaxEvents = GALAY_SCHEDULER_MAX_EVENTS,
    size_t BatchSize = GALAY_SCHEDULER_BATCH_SIZE,
    size_t QueueDepth = GALAY_SCHEDULER_QUEUE_DEPTH
>
struct IOSchedulerConfig {
    static constexpr size_t kMaxEvents = MaxEvents;
    static constexpr size_t kBatchSize = BatchSize;
    static constexpr size_t kQueueDepth = QueueDepth;

    static_assert(MaxEvents > 0 && MaxEvents <= 65536,
                  "MaxEvents must be in range [1, 65536]");
    static_assert(BatchSize > 0 && BatchSize <= 65536,
                  "BatchSize must be in range [1, 65536]");
    static_assert(QueueDepth > 0 && QueueDepth <= 65536,
                  "QueueDepth must be in range [1, 65536]");
};

/**
 * @brief 默认 IO 调度器配置
 * @details 使用编译期宏作为默认值，允许外部覆盖
 */
using DefaultIOSchedulerConfig = IOSchedulerConfig<>;

/**
 * @brief 高性能 IO 调度器配置
 * @details 更大的事件和批处理容量，适用于高吞吐场景
 */
using HighPerformanceIOSchedulerConfig = IOSchedulerConfig<2048, 512, 8192>;

/**
 * @brief 低延迟 IO 调度器配置
 * @details 更小的批处理大小，降低延迟
 */
using LowLatencyIOSchedulerConfig = IOSchedulerConfig<512, 64, 2048>;

/**
 * @brief 并行调度器配置模板
 * @tparam QueueCapacity 任务队列容量（预分配）
 */
template <size_t QueueCapacity = 4096>
struct ParallelSchedulerConfig {
    static constexpr size_t kQueueCapacity = QueueCapacity;

    static_assert(QueueCapacity > 0 && QueueCapacity <= 1048576,
                  "QueueCapacity must be in range [1, 1048576]");
};

/**
 * @brief 默认并行调度器配置
 */
using DefaultParallelSchedulerConfig = ParallelSchedulerConfig<>;

} // namespace galay::kernel

#endif // GALAY_KERNEL_SCHEDULER_CONFIG_H
