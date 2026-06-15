/**
 * @file trace_context.h
 * @brief 追踪上下文核心类型定义
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 定义分布式追踪的三个核心上下文类型：
 * - TraceContext：完整的追踪传播上下文，包含 tracestate
 * - SpanContext：轻量级 Span 身份信息，用于热路径
 * - LogContext：用于日志记录的最小追踪身份
 * 以及用于类型转换的工具函数。
 */

#pragma once

#include "galay-tracing/common/span_id.h"
#include "galay-tracing/common/trace_id.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace galay::tracing {

/**
 * @brief 完整的追踪传播上下文
 * @details 包含 TraceId、SpanId、追踪标志位、tracestate 和父 SpanId，
 * 用于跨服务边界传播和 Span 导出。携带 W3C tracestate 不透明字符串。
 */
class TraceContext {
public:
    TraceContext() = default;

    /**
     * @brief 构造追踪上下文
     * @param traceId 追踪标识符
     * @param spanId Span 标识符
     * @param traceFlags 追踪标志位（默认 0，位 0 表示采样）
     * @param tracestate W3C tracestate 字符串（默认为空）
     */
    TraceContext(TraceId traceId, SpanId spanId, std::uint8_t traceFlags = 0, std::string tracestate = {})
        : m_traceId(traceId),
          m_spanId(spanId),
          m_traceFlags(traceFlags),
          m_tracestate(std::move(tracestate)) {
    }

    /**
     * @brief 检查上下文是否有效（TraceId 和 SpanId 均有效）
     * @return 有效返回 true
     */
    [[nodiscard]] bool isValid() const noexcept {
        return m_traceId.isValid() && m_spanId.isValid();
    }

    /**
     * @brief 获取追踪标识符
     * @return TraceId 的常量引用
     */
    [[nodiscard]] const TraceId& traceId() const noexcept {
        return m_traceId;
    }

    /**
     * @brief 获取 Span 标识符
     * @return SpanId 的常量引用
     */
    [[nodiscard]] const SpanId& spanId() const noexcept {
        return m_spanId;
    }

    /**
     * @brief 获取追踪标志位
     * @return 追踪标志位的原始值
     */
    [[nodiscard]] std::uint8_t traceFlags() const noexcept {
        return m_traceFlags;
    }

    /**
     * @brief 检查是否已采样
     * @return 已采样返回 true
     */
    [[nodiscard]] bool sampled() const noexcept {
        return (m_traceFlags & kSampledFlag) != 0;
    }

    /**
     * @brief 获取 W3C tracestate 字符串
     * @return tracestate 的常量引用
     */
    [[nodiscard]] const std::string& tracestate() const noexcept {
        return m_tracestate;
    }

    /**
     * @brief 获取父 Span 标识符
     * @return 父 SpanId 的可选值
     */
    [[nodiscard]] const std::optional<SpanId>& parentSpanId() const noexcept {
        return m_parentSpanId;
    }

    /**
     * @brief 设置 Span 标识符
     * @param spanId 新的 SpanId
     */
    void setSpanId(SpanId spanId) noexcept {
        m_spanId = spanId;
    }

    /**
     * @brief 设置父 Span 标识符
     * @param parentSpanId 父 SpanId 的可选值
     */
    void setParentSpanId(std::optional<SpanId> parentSpanId) {
        m_parentSpanId = std::move(parentSpanId);
    }

    /**
     * @brief 设置追踪标志位
     * @param traceFlags 新的标志位值
     */
    void setTraceFlags(std::uint8_t traceFlags) noexcept {
        m_traceFlags = traceFlags;
    }

    /**
     * @brief 设置 W3C tracestate 字符串
     * @param tracestate 新的 tracestate 值
     */
    void setTracestate(std::string tracestate) {
        m_tracestate = std::move(tracestate);
    }

    friend bool operator==(const TraceContext&, const TraceContext&) = default;

private:
    static constexpr std::uint8_t kSampledFlag = 0x01; ///< 采样标志位掩码

    TraceId m_traceId{};                       ///< 追踪标识符
    SpanId m_spanId{};                         ///< Span 标识符
    std::optional<SpanId> m_parentSpanId;      ///< 父 Span 标识符
    std::uint8_t m_traceFlags{0};              ///< 追踪标志位
    std::string m_tracestate;                  ///< W3C tracestate
};

/**
 * @brief 轻量级 Span 身份信息
 * @details 用于热路径的 Span 身份表示，不携带 tracestate 字符串。
 * tracestate 仅在传播和导出 API 需要时才复制到 TraceContext 中。
 */
class SpanContext {
public:
    SpanContext() = default;

    /**
     * @brief 构造 Span 上下文
     * @param traceId 追踪标识符
     * @param spanId Span 标识符
     * @param traceFlags 追踪标志位（默认 0）
     * @param parentSpanId 父 SpanId（默认为空）
     */
    explicit constexpr SpanContext(
        TraceId traceId,
        SpanId spanId,
        std::uint8_t traceFlags = 0,
        std::optional<SpanId> parentSpanId = std::nullopt) noexcept
        : m_traceId(traceId),
          m_spanId(spanId),
          m_parentSpanId(parentSpanId),
          m_traceFlags(traceFlags) {
    }

    /**
     * @brief 从 TraceContext 构造 SpanContext（省略 tracestate）
     * @param context 源追踪上下文
     */
    explicit SpanContext(const TraceContext& context) noexcept
        : SpanContext(context.traceId(), context.spanId(), context.traceFlags(), context.parentSpanId()) {
    }

    /**
     * @brief 检查上下文是否有效
     * @return TraceId 和 SpanId 均有效时返回 true
     */
    [[nodiscard]] bool isValid() const noexcept {
        return m_traceId.isValid() && m_spanId.isValid();
    }

    /**
     * @brief 获取追踪标识符
     * @return TraceId 的常量引用
     */
    [[nodiscard]] const TraceId& traceId() const noexcept {
        return m_traceId;
    }

    /**
     * @brief 获取 Span 标识符
     * @return SpanId 的常量引用
     */
    [[nodiscard]] const SpanId& spanId() const noexcept {
        return m_spanId;
    }

    /**
     * @brief 获取父 Span 标识符
     * @return 父 SpanId 的可选值
     */
    [[nodiscard]] const std::optional<SpanId>& parentSpanId() const noexcept {
        return m_parentSpanId;
    }

    /**
     * @brief 获取追踪标志位
     * @return 追踪标志位的原始值
     */
    [[nodiscard]] std::uint8_t traceFlags() const noexcept {
        return m_traceFlags;
    }

    /**
     * @brief 检查是否已采样
     * @return 已采样返回 true
     */
    [[nodiscard]] bool sampled() const noexcept {
        return (m_traceFlags & kSampledFlag) != 0;
    }

    /**
     * @brief 设置 Span 标识符
     * @param spanId 新的 SpanId
     */
    void setSpanId(SpanId spanId) noexcept {
        m_spanId = spanId;
    }

    /**
     * @brief 设置父 Span 标识符
     * @param parentSpanId 父 SpanId 的可选值
     */
    void setParentSpanId(std::optional<SpanId> parentSpanId) noexcept {
        m_parentSpanId = parentSpanId;
    }

    /**
     * @brief 设置追踪标志位
     * @param traceFlags 新的标志位值
     */
    void setTraceFlags(std::uint8_t traceFlags) noexcept {
        m_traceFlags = traceFlags;
    }

    /**
     * @brief 转换为完整的 TraceContext
     * @param tracestate W3C tracestate 字符串（默认为空）
     * @return 包含给定 tracestate 的完整 TraceContext
     */
    [[nodiscard]] TraceContext toTraceContext(std::string tracestate = {}) const {
        TraceContext context(m_traceId, m_spanId, m_traceFlags, std::move(tracestate));
        context.setParentSpanId(m_parentSpanId);
        return context;
    }

    friend bool operator==(const SpanContext&, const SpanContext&) noexcept = default;

private:
    static constexpr std::uint8_t kSampledFlag = 0x01; ///< 采样标志位掩码

    TraceId m_traceId{};                       ///< 追踪标识符
    SpanId m_spanId{};                         ///< Span 标识符
    std::optional<SpanId> m_parentSpanId;      ///< 父 Span 标识符
    std::uint8_t m_traceFlags{0};              ///< 追踪标志位
};

/**
 * @brief 轻量级日志追踪身份
 * @details 复制到日志记录中的最小追踪身份信息，故意省略 tracestate 和父 SpanId。
 * 传播和导出操作仍使用 TraceContext。
 */
class LogContext {
public:
    LogContext() = default;

    /**
     * @brief 构造日志上下文
     * @param traceId 追踪标识符
     * @param spanId Span 标识符
     * @param traceFlags 追踪标志位（默认 0）
     */
    explicit constexpr LogContext(TraceId traceId, SpanId spanId, std::uint8_t traceFlags = 0) noexcept
        : m_traceId(traceId),
          m_spanId(spanId),
          m_traceFlags(traceFlags) {
    }

    /**
     * @brief 从 TraceContext 构造 LogContext
     * @param context 源追踪上下文
     */
    explicit LogContext(const TraceContext& context) noexcept
        : LogContext(context.traceId(), context.spanId(), context.traceFlags()) {
    }

    /**
     * @brief 检查上下文是否有效
     * @return TraceId 和 SpanId 均有效时返回 true
     */
    [[nodiscard]] bool isValid() const noexcept {
        return m_traceId.isValid() && m_spanId.isValid();
    }

    /**
     * @brief 获取追踪标识符
     * @return TraceId 的常量引用
     */
    [[nodiscard]] const TraceId& traceId() const noexcept {
        return m_traceId;
    }

    /**
     * @brief 获取 Span 标识符
     * @return SpanId 的常量引用
     */
    [[nodiscard]] const SpanId& spanId() const noexcept {
        return m_spanId;
    }

    /**
     * @brief 获取追踪标志位
     * @return 追踪标志位的原始值
     */
    [[nodiscard]] std::uint8_t traceFlags() const noexcept {
        return m_traceFlags;
    }

    /**
     * @brief 检查是否已采样
     * @return 已采样返回 true
     */
    [[nodiscard]] bool sampled() const noexcept {
        return (m_traceFlags & kSampledFlag) != 0;
    }

    friend bool operator==(const LogContext&, const LogContext&) noexcept = default;

private:
    static constexpr std::uint8_t kSampledFlag = 0x01; ///< 采样标志位掩码

    TraceId m_traceId{};                ///< 追踪标识符
    SpanId m_spanId{};                  ///< Span 标识符
    std::uint8_t m_traceFlags{0};       ///< 追踪标志位
};

/**
 * @brief 从可选的 TraceContext 创建 LogContext
 * @param context 可选的追踪上下文
 * @return 包含 LogContext 的可选值，输入为空时返回空
 */
[[nodiscard]] inline std::optional<LogContext> makeLogContext(const std::optional<TraceContext>& context) {
    if (!context.has_value()) {
        return std::nullopt;
    }
    return LogContext(*context);
}

/**
 * @brief 从右值可选 TraceContext 创建 LogContext
 * @param context 可选的追踪上下文右值
 * @return 包含 LogContext 的可选值
 */
[[nodiscard]] inline std::optional<LogContext> makeLogContext(std::optional<TraceContext>&& context) {
    return makeLogContext(context);
}

/**
 * @brief 从 TraceContext 引用创建 LogContext
 * @param context 追踪上下文引用
 * @return 对应的 LogContext
 */
[[nodiscard]] inline std::optional<LogContext> makeLogContext(const TraceContext& context) {
    return LogContext(context);
}

/**
 * @brief 从 std::nullopt 创建空的 LogContext
 * @return 空的可选值
 */
[[nodiscard]] constexpr std::optional<LogContext> makeLogContext(std::nullopt_t) noexcept {
    return std::nullopt;
}

} // namespace galay::tracing
