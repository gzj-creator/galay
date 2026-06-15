/**
 * @file sampler.h
 * @brief 追踪采样器接口与内置实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 定义采样器抽象接口及四种内置采样策略：始终采样、始终不采样、
 * 基于父 Span 的采样和基于 TraceId 比例的采样。支持全局采样器配置。
 */

#pragma once

#include "galay-tracing/context/trace_context.h"

#include <cstdint>

namespace galay::tracing {

/**
 * @brief 采样器抽象基类
 * @details 定义 Span 采样决策接口。采样器根据父 Span 上下文和 TraceId
 * 决定是否采样新创建的 Span。
 */
class Sampler {
public:
    virtual ~Sampler() = default;

    /**
     * @brief 判断是否应该采样给定的 Span
     * @param parent 父 Span 的上下文，无父 Span 时为 nullptr
     * @param traceId 待采样 Span 的 TraceId
     * @return 应该采样返回 true，否则返回 false
     */
    [[nodiscard]] virtual bool shouldSample(const SpanContext* parent, const TraceId& traceId) const noexcept = 0;
};

/**
 * @brief 始终采样器
 * @details 对所有 Span 都返回采样决策为 true。
 */
class AlwaysOnSampler final : public Sampler {
public:
    /**
     * @brief 始终返回 true
     */
    [[nodiscard]] bool shouldSample(const SpanContext* parent, const TraceId& traceId) const noexcept override;
};

/**
 * @brief 始终不采样器
 * @details 对所有 Span 都返回采样决策为 false。
 */
class AlwaysOffSampler final : public Sampler {
public:
    /**
     * @brief 始终返回 false
     */
    [[nodiscard]] bool shouldSample(const SpanContext* parent, const TraceId& traceId) const noexcept override;
};

/**
 * @brief 基于父 Span 的采样器
 * @details 如果存在父 Span，则跟随父 Span 的采样决策；
 * 如果不存在父 Span，则委托给根采样器（rootSampler）决策。
 */
class ParentBasedSampler final : public Sampler {
public:
    /**
     * @brief 构造基于父 Span 的采样器
     * @param rootSampler 无父 Span 时使用的根采样器引用（调用方须保证生命周期）
     */
    explicit ParentBasedSampler(const Sampler& rootSampler) noexcept;

    /**
     * @brief 根据父 Span 采样决策或根采样器决定是否采样
     */
    [[nodiscard]] bool shouldSample(const SpanContext* parent, const TraceId& traceId) const noexcept override;

private:
    const Sampler* m_rootSampler; ///< 根采样器指针（不拥有所有权）
};

/**
 * @brief 基于 TraceId 比例的采样器
 * @details 根据 TraceId 的数值与给定比例决定是否采样，
 * 比例范围 [0.0, 1.0]，确保相同 TraceId 的采样决策一致。
 */
class TraceIdRatioSampler final : public Sampler {
public:
    /**
     * @brief 构造比例采样器
     * @param ratio 采样比例，范围 [0.0, 1.0]
     */
    explicit TraceIdRatioSampler(double ratio) noexcept;

    /**
     * @brief 获取采样比例
     * @return 当前采样比例值
     */
    [[nodiscard]] double ratio() const noexcept {
        return m_ratio;
    }

    /**
     * @brief 根据 TraceId 数值和比例决定是否采样
     */
    [[nodiscard]] bool shouldSample(const SpanContext* parent, const TraceId& traceId) const noexcept override;

private:
    double m_ratio{1.0}; ///< 采样比例
};

/**
 * @brief 设置进程级全局采样器
 * @details 不获取所有权，传入 nullptr 恢复为内置的基于父 Span 的采样器
 * （使用始终采样作为根决策）。
 * @param sampler 采样器指针（不拥有所有权）
 */
void setSampler(const Sampler* sampler) noexcept;

/**
 * @brief 获取当前进程级全局采样器
 * @return 当前采样器的常量引用
 */
[[nodiscard]] const Sampler& currentSampler() noexcept;

} // namespace galay::tracing
