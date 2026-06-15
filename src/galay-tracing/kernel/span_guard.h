/**
 * @file span_guard.h
 * @brief Span RAII 守卫与自动 Span 创建函数
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 提供 SpanGuard RAII 守卫，在构造时自动创建 Span 并设置当前线程上下文，
 * 析构时自动结束 Span 并恢复之前的上下文。同时提供 startSpan 和 startServerSpan
 * 便捷函数用于创建不同类型的 Span。
 */

#pragma once

#include "galay-tracing/kernel/span.h"

#include <optional>
#include <string>
#include <string_view>

namespace galay::tracing {

/**
 * @brief Span 的 RAII 守卫
 * @details 在构造时保存之前的追踪上下文并激活新 Span 的上下文，
 * 析构时自动结束 Span 并恢复之前的上下文。支持移动语义，禁止复制。
 */
class SpanGuard {
public:
    SpanGuard() noexcept = default;

    /**
     * @brief 构造 Span 守卫
     * @param span 要守卫的 Span
     * @param previousContext 之前的 SpanContext（用于析构时恢复）
     * @param previousTracestate 之前的 tracestate（用于析构时恢复）
     */
    SpanGuard(Span span, std::optional<SpanContext> previousContext, std::string previousTracestate);

    /**
     * @brief 析构时自动结束 Span 并恢复之前的上下文
     */
    ~SpanGuard() noexcept;

    SpanGuard(const SpanGuard&) = delete;
    SpanGuard& operator=(const SpanGuard&) = delete;

    /**
     * @brief 移动构造 Span 守卫
     * @param other 源守卫（移动后源守卫变为非活跃状态）
     */
    SpanGuard(SpanGuard&& other) noexcept;

    /**
     * @brief 移动赋值 Span 守卫
     * @param other 源守卫
     * @return 当前守卫的引用
     */
    SpanGuard& operator=(SpanGuard&& other) noexcept;

    /**
     * @brief 获取被守卫的 Span
     * @return Span 的常量引用
     */
    [[nodiscard]] const Span& span() const noexcept {
        return m_span;
    }

    /**
     * @brief 检查守卫是否仍然活跃
     * @return 活跃返回 true，已结束或已移动返回 false
     */
    [[nodiscard]] bool active() const noexcept {
        return m_active;
    }

    /**
     * @brief 手动结束 Span 并恢复之前的上下文
     */
    void end() noexcept;

private:
    /**
     * @brief 恢复之前的追踪上下文
     */
    void restore() noexcept;

    Span m_span;                              ///< 被守卫的 Span
    std::optional<SpanContext> m_previousContext; ///< 之前的 Span 上下文
    std::string m_previousTracestate;        ///< 之前的 tracestate
    bool m_active{false};                    ///< 守卫是否活跃
};

/**
 * @brief 创建并激活一个新的内部 Span
 * @details 自动采样、创建 Span，设置当前线程上下文，返回 RAII 守卫。
 * @param name Span 操作名称
 * @return 守卫新创建 Span 的 SpanGuard
 */
[[nodiscard]] SpanGuard startSpan(std::string_view name);

/**
 * @brief 创建并激活一个新的服务端 Span（带父上下文）
 * @details 用于服务端接收到外部请求时，从父上下文创建子 Span。
 * @param name Span 操作名称
 * @param parent 父 Span 的追踪上下文
 * @return 守卫新创建 Span 的 SpanGuard
 */
[[nodiscard]] SpanGuard startServerSpan(std::string_view name, const TraceContext& parent);

} // namespace galay::tracing
