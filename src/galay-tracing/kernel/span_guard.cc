/**
 * @file span_guard.cc
 * @brief Span RAII 守卫与自动 Span 创建函数实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现 SpanGuard 的移动语义、上下文保存/恢复逻辑，
 * 以及 startSpan 和 startServerSpan 便捷函数中的采样决策和上下文传播。
 */

#include "galay-tracing/kernel/span_guard.h"

#include "galay-tracing/context/context_storage.h"
#include "galay-tracing/kernel/sampler.h"

#include <cstdint>
#include <string>
#include <utility>

namespace galay::tracing {

namespace {

constexpr std::uint8_t kSampledFlag = 0x01;

[[nodiscard]] SpanContext makeRootContext() {
    return SpanContext(TraceId::random(), SpanId::random());
}

[[nodiscard]] SpanContext makeChildContext(const SpanContext& parent) {
    SpanContext context(parent.traceId(), SpanId::random(), parent.traceFlags());
    context.setParentSpanId(parent.spanId());
    return context;
}

[[nodiscard]] SpanContext makeChildContext(const TraceContext& parent) {
    SpanContext context(parent.traceId(), SpanId::random(), parent.traceFlags());
    context.setParentSpanId(parent.spanId());
    return context;
}

[[nodiscard]] SpanContext makeNextContext(const std::optional<SpanContext>& parent) {
    if (parent.has_value() && parent->isValid()) {
        return makeChildContext(*parent);
    }
    return makeRootContext();
}

void applySampling(SpanContext& context, const SpanContext* parent) noexcept {
    auto flags = context.traceFlags();
    if (currentSampler().shouldSample(parent, context.traceId())) {
        flags |= kSampledFlag;
    } else {
        flags &= static_cast<std::uint8_t>(~kSampledFlag);
    }
    context.setTraceFlags(flags);
}

} // namespace

SpanGuard::SpanGuard(Span span, std::optional<SpanContext> previousContext, std::string previousTracestate)
    : m_span(std::move(span)),
      m_previousContext(std::move(previousContext)),
      m_previousTracestate(std::move(previousTracestate)),
      m_active(true) {
}

SpanGuard::~SpanGuard() noexcept {
    restore();
}

SpanGuard::SpanGuard(SpanGuard&& other) noexcept
    : m_span(std::move(other.m_span)),
      m_previousContext(std::move(other.m_previousContext)),
      m_previousTracestate(std::move(other.m_previousTracestate)),
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
    try {
        detail::setCurrentContextState(detail::CurrentContextState{
            .spanContext = std::move(m_previousContext),
            .tracestate = std::move(m_previousTracestate),
        });
    } catch (...) {
        clearCurrentContext();
    }
    m_active = false;
}

SpanGuard startSpan(std::string_view name) {
    auto previous = detail::currentContextState();
    const auto* parent = previous.spanContext.has_value() && previous.spanContext->isValid()
        ? &*previous.spanContext
        : nullptr;
    auto context = makeNextContext(previous.spanContext);
    applySampling(context, parent);
    auto tracestate = previous.spanContext.has_value() ? previous.tracestate : std::string();
    Span span(std::string(name), context, tracestate);
    detail::setCurrentContextState(detail::CurrentContextState{
        .spanContext = context,
        .tracestate = tracestate,
    });
    return SpanGuard(std::move(span), std::move(previous.spanContext), std::move(previous.tracestate));
}

SpanGuard startServerSpan(std::string_view name, const TraceContext& parent) {
    auto previous = detail::currentContextState();
    auto context = parent.isValid() ? makeChildContext(parent) : makeRootContext();
    auto samplingParent = parent.isValid() ? std::optional<SpanContext>(SpanContext(parent)) : std::nullopt;
    applySampling(context, samplingParent.has_value() ? &*samplingParent : nullptr);
    auto tracestate = parent.isValid() ? parent.tracestate() : std::string();
    Span span(std::string(name), context, tracestate);
    span.setKind(SpanKind::kServer);
    detail::setCurrentContextState(detail::CurrentContextState{
        .spanContext = context,
        .tracestate = tracestate,
    });
    return SpanGuard(std::move(span), std::move(previous.spanContext), std::move(previous.tracestate));
}

} // namespace galay::tracing
