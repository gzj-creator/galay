/**
 * @file context_storage.cc
 * @brief 线程本地追踪上下文的存储与访问实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 基于 thread_local 实现当前线程活跃 TraceContext 的获取、设置和清除，
 * 支持 SpanContext 与 tracestate 的联合序列化状态管理。
 */

#include "galay-tracing/context/context_storage.h"

#include <utility>

namespace galay::tracing {

namespace {

thread_local detail::CurrentContextState t_currentContext;

} // namespace

namespace detail {

CurrentContextState currentContextState() {
    return t_currentContext;
}

void setCurrentContextState(CurrentContextState state) {
    if (!state.spanContext.has_value()) {
        state.tracestate.clear();
    }
    t_currentContext = std::move(state);
}

} // namespace detail

std::optional<TraceContext> currentContext() noexcept {
    if (!t_currentContext.spanContext.has_value()) {
        return std::nullopt;
    }
    return t_currentContext.spanContext->toTraceContext(t_currentContext.tracestate);
}

void setCurrentContext(std::optional<TraceContext> context) {
    if (!context.has_value()) {
        clearCurrentContext();
        return;
    }
    t_currentContext = detail::CurrentContextState{
        .spanContext = SpanContext(*context),
        .tracestate = context->tracestate(),
    };
}

void clearCurrentContext() noexcept {
    t_currentContext.spanContext.reset();
    t_currentContext.tracestate.clear();
}

} // namespace galay::tracing
