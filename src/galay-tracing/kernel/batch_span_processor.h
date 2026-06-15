/**
 * @file batch_span_processor.h
 * @brief 批量 Span 处理器
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现基于后台线程的批量 Span 处理器，将已结束的 Span 缓冲到队列中，
 * 按批次大小或定时间隔批量导出到后端。支持队列溢出丢弃统计和优雅关闭。
 */

#pragma once

#include "galay-tracing/kernel/span_exporter.h"
#include "galay-tracing/kernel/span_processor.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace galay::tracing {

/**
 * @brief 批处理器后台线程的唤醒策略
 */
enum class BatchSpanScheduleMode {
    kTimed,      ///< 仅按 flush_interval 定时唤醒，forceFlush/shutdown 仍会立即唤醒
    kOnEnd,      ///< 每次成功入队一个 sampled Span 后唤醒后台线程
    kBatchSize,  ///< 队列中 Span 数量达到 max_batch_size 后唤醒后台线程
};

/**
 * @brief 批量处理器的配置参数
 */
struct BatchSpanProcessorConfig {
    std::size_t queue_capacity{2048};  ///< 内部队列容量，超出时丢弃新 Span
    std::size_t max_batch_size{512};   ///< 单次导出的最大批大小
    std::chrono::milliseconds flush_interval{std::chrono::milliseconds(5000)}; ///< 定时刷新间隔
    BatchSpanScheduleMode schedule_mode{BatchSpanScheduleMode::kBatchSize}; ///< 后台线程唤醒策略
};

/**
 * @brief 批量 Span 处理器
 * @details 使用后台工作线程从内部队列中批量取出已结束的 Span，
 * 按配置的批大小或刷新间隔调用 SpanExporter 进行导出。
 * 队列满时丢弃新 Span 并统计丢弃数量。禁止复制。
 */
class BatchSpanProcessor final : public SpanProcessor {
public:
    /**
     * @brief 构造批量处理器
     * @param exporter Span 导出器的独占指针
     * @param config 批处理配置（使用默认值或自定义）
     */
    explicit BatchSpanProcessor(std::unique_ptr<SpanExporter> exporter, BatchSpanProcessorConfig config = {});

    /**
     * @brief 析构时关闭工作线程并导出剩余 Span
     */
    ~BatchSpanProcessor() noexcept override;

    BatchSpanProcessor(const BatchSpanProcessor&) = delete;
    BatchSpanProcessor& operator=(const BatchSpanProcessor&) = delete;

    /**
     * @brief 将已结束的 Span 加入导出队列
     * @details 队列满时丢弃 Span 并增加丢弃计数
     * @param span 已结束的 Span
     */
    void onEnd(Span&& span) override;

    /**
     * @brief 强制刷新队列中所有 Span
     * @param timeout 超时时间
     * @return 成功刷新返回 true
     */
    bool forceFlush(std::chrono::milliseconds timeout) override;

    /**
     * @brief 关闭处理器并释放资源
     * @param timeout 超时时间
     * @return 成功关闭返回 true
     */
    bool shutdown(std::chrono::milliseconds timeout) override;

    /**
     * @brief 获取因队列满而丢弃的 Span 数量
     * @return 丢弃的 Span 总数
     */
    [[nodiscard]] std::size_t droppedSpanCount() const noexcept;

private:
    struct SpanQueue;
    struct WorkerControl;

    /**
     * @brief 后台工作线程的主循环
     */
    void workerLoop();

    /**
     * @brief 从队列中取出最多 maxCount 个 Span
     * @param batch 接收取出结果的复用向量，前返回值个元素为本次取出的 Span
     * @param maxCount 最大取出数量
     * @return 实际取出的 Span 数量
     */
    [[nodiscard]] std::size_t drainQueue(std::vector<Span>& batch, std::size_t maxCount);

    /**
     * @brief 将一批 Span 导出到后端
     * @param spans 待导出的 Span 视图
     * @return 导出成功返回 true
     */
    [[nodiscard]] bool exportBatch(std::span<const Span> spans);

    std::unique_ptr<SpanExporter> m_exporter;       ///< Span 导出器
    BatchSpanProcessorConfig m_config;              ///< 批处理配置
    std::unique_ptr<SpanQueue> m_queue;             ///< 基于 moodycamel ConcurrentQueue 的 Span 队列
    std::unique_ptr<WorkerControl> m_control;       ///< 后台线程的原子控制通道
    std::atomic<std::size_t> m_droppedSpans{0};     ///< 丢弃的 Span 计数
    std::thread m_worker;                           ///< 后台工作线程
};

} // namespace galay::tracing
