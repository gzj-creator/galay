/**
 * @file context_storage.h
 * @brief 线程本地追踪上下文的存储与访问接口
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 提供线程本地的追踪上下文存储机制，支持获取、设置和清除当前线程的
 * 活跃 TraceContext。上下文传播和 Span 守卫依赖此模块实现自动上下文管理。
 */

#pragma once

#include "galay-tracing/context/trace_context.h"

#include <optional>
#include <string>

namespace galay::tracing {

namespace detail {

/**
 * @brief 当前线程的上下文状态快照
 * @details 包含 SpanContext 和 tracestate，用于内部序列化/反序列化线程本地存储
 */
struct CurrentContextState {
    std::optional<SpanContext> spanContext; ///< 当前活跃的 Span 上下文
    std::string tracestate;                 ///< W3C tracestate 不透明字符串
};

/**
 * @brief 获取当前线程的上下文状态快照
 * @return 当前线程的 CurrentContextState
 */
[[nodiscard]] CurrentContextState currentContextState();

/**
 * @brief 设置当前线程的上下文状态
 * @param state 要设置的上下文状态
 */
void setCurrentContextState(CurrentContextState state);

} // namespace detail

/**
 * @brief 获取当前线程的活跃追踪上下文
 * @return 当前线程的 TraceContext，若未设置则返回空
 */
[[nodiscard]] std::optional<TraceContext> currentContext() noexcept;

/**
 * @brief 设置当前线程的活跃追踪上下文
 * @param context 要设置的追踪上下文，传入空值表示清除
 */
void setCurrentContext(std::optional<TraceContext> context);

/**
 * @brief 设置当前线程的活跃追踪上下文（便捷重载）
 * @param context 要设置的追踪上下文引用
 */
inline void setCurrentContext(const TraceContext& context) {
    setCurrentContext(std::optional<TraceContext>(context));
}

/**
 * @brief 清除当前线程的活跃追踪上下文
 */
void clearCurrentContext() noexcept;

} // namespace galay::tracing
