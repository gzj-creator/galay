/**
 * @file span_guard.cc
 * @brief Span RAII 守卫与自动 Span 创建函数实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现 SpanGuard 的移动语义、上下文保存/恢复逻辑，
 * 以及 start_span 和 start_server_span 便捷函数中的采样决策和上下文传播。
 */

#include "span_guard.h"

#include "../context/context_storage.h"
#include "sampler.h"
#include "tracer_provider.h"

#include <cstdint>
#include <string>
#include <utility>

namespace galay::tracing {

namespace {

constexpr std::uint8_t kSampledFlag = 0x01;

[[nodiscard]] SpanContext make_root_context() {
    return SpanContext(TraceId::random(), SpanId::random());
}

[[nodiscard]] SpanContext make_child_context(const SpanContext& parent) {
    SpanContext context(parent.trace_id(), SpanId::random(), parent.trace_flags());
    context.set_parent_span_id(parent.span_id());
    return context;
}

[[nodiscard]] SpanContext make_child_context(const TraceContext& parent) {
    SpanContext context(parent.trace_id(), SpanId::random(), parent.trace_flags());
    context.set_parent_span_id(parent.span_id());
    return context;
}

[[nodiscard]] SpanContext make_next_context(const std::optional<SpanContext>& parent) {
    if (parent.has_value() && parent->is_valid()) {
        return make_child_context(*parent);
    }
    return make_root_context();
}

void apply_sampling(SpanContext& context, const SpanContext* parent) noexcept {
    auto flags = context.trace_flags();
    if (current_sampler().should_sample(parent, context.trace_id())) {
        flags |= kSampledFlag;
    } else {
        flags &= static_cast<std::uint8_t>(~kSampledFlag);
    }
    context.set_trace_flags(flags);
}

} // namespace

SpanGuard::SpanGuard(Span span, std::optional<SpanContext> previousContext, std::string previousTracestate)
    : m_span(std::move(span)),
      m_previousTracestate(std::move(previousTracestate)),
      m_previousContext(std::move(previousContext)),
      m_active(true) {
}

SpanGuard::~SpanGuard() noexcept {
    restore();
}

SpanGuard::SpanGuard(SpanGuard&& other) noexcept
    : m_span(std::move(other.m_span)),
      m_previousTracestate(std::move(other.m_previousTracestate)),
      m_previousContext(std::move(other.m_previousContext)),
      m_active(other.m_active) {
    other.m_active = false;
}

SpanGuard& SpanGuard::operator=(SpanGuard&& other) noexcept {
    if (this != &other) {
        restore();
        m_span = std::move(other.m_span);
        m_previousContext = std::move(other.m_previousContext);
        m_previousTracestate = std::move(other.m_previousTracestate);
        m_active = other.m_active;
        other.m_active = false;
    }
    return *this;
}

void SpanGuard::end() noexcept {
    if (m_active) {
        m_span.end();
    }
}

void SpanGuard::restore() noexcept {
    if (!m_active) {
        return;
    }

    end();
    if (m_span.span_context().sampled()) {
        if (auto* processor = current_span_processor(); processor != nullptr) {
            try {
                processor->on_end(std::move(m_span));
            } catch (...) {
            }
        }
    }
    try {
        detail::set_current_context_state(detail::CurrentContextState{
            .tracestate = std::move(m_previousTracestate),
            .spanContext = std::move(m_previousContext),
        });
    } catch (...) {
        clear_current_context();
    }
    m_active = false;
}

SpanGuard start_span(std::string_view name) {
    auto previous = detail::current_context_state();
    const auto* parent = previous.spanContext.has_value() && previous.spanContext->is_valid()
        ? &*previous.spanContext
        : nullptr;
    auto context = make_next_context(previous.spanContext);
    apply_sampling(context, parent);
    auto tracestate = previous.spanContext.has_value() ? previous.tracestate : std::string();
    Span span(std::string(name), context, tracestate);
    detail::set_current_context_state(detail::CurrentContextState{
        .tracestate = tracestate,
        .spanContext = context,
    });
    return SpanGuard(std::move(span), std::move(previous.spanContext), std::move(previous.tracestate));
}

SpanGuard start_server_span(std::string_view name, const TraceContext& parent) {
    auto previous = detail::current_context_state();
    auto context = parent.is_valid() ? make_child_context(parent) : make_root_context();
    auto samplingParent = parent.is_valid() ? std::optional<SpanContext>(SpanContext(parent)) : std::nullopt;
    apply_sampling(context, samplingParent.has_value() ? &*samplingParent : nullptr);
    auto tracestate = parent.is_valid() ? parent.tracestate() : std::string();
    Span span(std::string(name), context, tracestate);
    span.set_kind(SpanKind::kServer);
    detail::set_current_context_state(detail::CurrentContextState{
        .tracestate = tracestate,
        .spanContext = context,
    });
    return SpanGuard(std::move(span), std::move(previous.spanContext), std::move(previous.tracestate));
}

} // namespace galay::tracing
