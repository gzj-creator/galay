#include "galay-tracing/context/context_storage.h"
#include "galay-tracing/kernel/sampler.h"
#include "galay-tracing/kernel/span_guard.h"

#include <cassert>
#include <type_traits>
#include <utility>

namespace {

static_assert(sizeof(galay::tracing::SpanContext) < sizeof(galay::tracing::TraceContext));
static_assert(std::is_same_v<
    decltype(std::declval<const galay::tracing::Span&>().spanContext()),
    const galay::tracing::SpanContext&>);
static_assert(std::is_same_v<
    decltype(std::declval<const galay::tracing::Span&>().context()),
    galay::tracing::TraceContext>);

class SpanTimingPolicyScope {
public:
    explicit SpanTimingPolicyScope(galay::tracing::SpanTimingPolicy policy)
        : m_previous(galay::tracing::spanTimingPolicy()) {
        galay::tracing::setSpanTimingPolicy(policy);
    }

    ~SpanTimingPolicyScope() {
        galay::tracing::setSpanTimingPolicy(m_previous);
    }

private:
    galay::tracing::SpanTimingPolicy m_previous;
};

class SamplerScope {
public:
    explicit SamplerScope(const galay::tracing::Sampler* sampler) noexcept {
        galay::tracing::setSampler(sampler);
    }

    ~SamplerScope() {
        galay::tracing::setSampler(nullptr);
    }
};

void spanContextRoundTripsTraceIdentity() {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
    const auto parent = galay::tracing::SpanId::fromHex("1111111111111111");
    context.setParentSpanId(parent);

    const auto spanContext = galay::tracing::SpanContext(context);
    const auto roundTrip = spanContext.toTraceContext(context.tracestate());

    assert(spanContext.isValid());
    assert(spanContext.traceId() == context.traceId());
    assert(spanContext.spanId() == context.spanId());
    assert(spanContext.parentSpanId().has_value());
    assert(*spanContext.parentSpanId() == parent);
    assert(spanContext.sampled());
    assert(roundTrip == context);
}

void spanStoresLightweightContextAndBuildsPropagationContext() {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
    const auto parent = galay::tracing::SpanId::fromHex("1111111111111111");
    context.setParentSpanId(parent);

    const auto spanContext = galay::tracing::SpanContext(context);
    galay::tracing::Span span("fast", spanContext, context.tracestate(), galay::tracing::SpanTimingPolicy::kDisabled);
    const auto propagation = span.context();

    assert(span.spanContext() == spanContext);
    assert(propagation == context);
}

void spanStoresSemanticFieldsAndBoundsAttributes() {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01);
    galay::tracing::Span span("semantic", context);

    assert(span.kind() == galay::tracing::SpanKind::kInternal);
    span.setKind(galay::tracing::SpanKind::kClient);
    assert(span.kind() == galay::tracing::SpanKind::kClient);

    assert(span.status().code == galay::tracing::SpanStatusCode::kUnset);
    span.setStatus(galay::tracing::SpanStatusCode::kError, "timeout");
    assert(span.status().code == galay::tracing::SpanStatusCode::kError);
    assert(span.status().message == "timeout");

    assert(span.setAttribute("http.method", "GET"));
    assert(span.setAttribute("http.status_code", 503));
    assert(span.setAttribute("retry", true));
    assert(span.setAttribute("latency_ms", 12.5));

    const auto attributes = span.attributes();
    assert(attributes.size() == 4);
    assert(attributes[0].name == "http.method");
    assert(attributes[0].value.type() == galay::tracing::SpanAttributeType::kString);
    assert(attributes[0].value.asString() == "GET");
    assert(attributes[1].value.asInt64() == 503);
    assert(attributes[2].value.asBool());
    assert(attributes[3].value.asDouble() == 12.5);

    for (std::size_t i = attributes.size(); i < galay::tracing::Span::kMaxAttributes; ++i) {
        assert(span.setAttribute("extra", static_cast<std::int64_t>(i)));
    }
    assert(!span.setAttribute("overflow", 1));
    assert(span.attributes().size() == galay::tracing::Span::kMaxAttributes);
}

void rootSpanCreatesAndRestoresContext() {
    galay::tracing::clearCurrentContext();
    galay::tracing::TraceId traceId;
    galay::tracing::SpanId spanId;

    {
        auto guard = galay::tracing::startSpan("root");
        auto current = galay::tracing::currentContext();

        assert(current.has_value());
        assert(current->traceId().isValid());
        assert(current->spanId().isValid());
        assert(!current->parentSpanId().has_value());
        assert(guard.span().name() == "root");
        assert(guard.span().kind() == galay::tracing::SpanKind::kInternal);

        traceId = current->traceId();
        spanId = current->spanId();
    }

    assert(traceId.isValid());
    assert(spanId.isValid());
    assert(!galay::tracing::currentContext().has_value());
}

void nestedSpanUsesParentTraceAndRestoresParent() {
    galay::tracing::clearCurrentContext();

    auto root = galay::tracing::startSpan("root");
    const auto rootContext = galay::tracing::currentContext();
    assert(rootContext.has_value());

    {
        auto child = galay::tracing::startSpan("child");
        const auto childContext = galay::tracing::currentContext();

        assert(childContext.has_value());
        assert(childContext->traceId() == rootContext->traceId());
        assert(childContext->spanId() != rootContext->spanId());
        assert(childContext->parentSpanId().has_value());
        assert(*childContext->parentSpanId() == rootContext->spanId());
        assert(child.span().name() == "child");
    }

    assert(galay::tracing::currentContext() == rootContext);
}

void startServerSpanUsesInboundParentAndRestoresPrevious() {
    auto inbound = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
    auto previous = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        galay::tracing::SpanId::fromHex("bbbbbbbbbbbbbbbb"));

    galay::tracing::setCurrentContext(previous);

    {
        auto server = galay::tracing::startServerSpan("server", inbound);
        const auto current = galay::tracing::currentContext();

        assert(current.has_value());
        assert(current->traceId() == inbound.traceId());
        assert(current->spanId() != inbound.spanId());
        assert(current->parentSpanId().has_value());
        assert(*current->parentSpanId() == inbound.spanId());
        assert(current->sampled());
        assert(current->tracestate() == inbound.tracestate());
        assert(server.span().name() == "server");
        assert(server.span().kind() == galay::tracing::SpanKind::kServer);
    }

    assert(galay::tracing::currentContext() == previous);
    galay::tracing::clearCurrentContext();
}

void defaultSpanTimingSkipsTiming() {
    const auto parent = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01);
    galay::tracing::setCurrentContext(parent);

    auto span = galay::tracing::startSpan("default_timing");
    assert(span.span().startedAt() == galay::tracing::Span::Clock::time_point{});
    span.end();
    assert(span.span().ended());
    assert(span.span().endedAt() == galay::tracing::Span::Clock::time_point{});

    galay::tracing::clearCurrentContext();
}

void enabledSpanTimingRecordsTiming() {
    SpanTimingPolicyScope timing{galay::tracing::SpanTimingPolicy::kEnabled};
    const auto parent = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01);
    galay::tracing::setCurrentContext(parent);

    auto span = galay::tracing::startSpan("sampled");
    assert(span.span().startedAt() != galay::tracing::Span::Clock::time_point{});
    span.end();
    assert(span.span().ended());
    assert(span.span().endedAt() != galay::tracing::Span::Clock::time_point{});
    assert(span.span().endedAt() >= span.span().startedAt());

    galay::tracing::clearCurrentContext();
}

void disabledSpanTimingRestoresLowCostBehavior() {
    SpanTimingPolicyScope timing{galay::tracing::SpanTimingPolicy::kDisabled};
    const auto parent = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01);
    galay::tracing::setCurrentContext(parent);

    auto span = galay::tracing::startSpan("disabled_timing");
    assert(span.span().startedAt() == galay::tracing::Span::Clock::time_point{});
    span.end();
    assert(span.span().endedAt() == galay::tracing::Span::Clock::time_point{});

    galay::tracing::clearCurrentContext();
}

void defaultSamplerSamplesRootSpan() {
    SamplerScope sampler(nullptr);
    galay::tracing::clearCurrentContext();

    auto span = galay::tracing::startSpan("root_sampled");

    assert(span.span().spanContext().sampled());
    assert(galay::tracing::currentContext()->sampled());
    galay::tracing::clearCurrentContext();
}

void configuredSamplerCanDropRootSpan() {
    galay::tracing::AlwaysOffSampler off;
    SamplerScope sampler(&off);
    galay::tracing::clearCurrentContext();

    auto span = galay::tracing::startSpan("root_unsampled");

    assert(!span.span().spanContext().sampled());
    assert(!galay::tracing::currentContext()->sampled());
    galay::tracing::clearCurrentContext();
}

void parentBasedSamplerInheritsRemoteDecision() {
    galay::tracing::AlwaysOffSampler rootOff;
    galay::tracing::ParentBasedSampler parentBased(rootOff);
    SamplerScope sampler(&parentBased);

    auto sampledParent = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01);
    auto unsampledParent = sampledParent;
    unsampledParent.setTraceFlags(0x00);

    {
        auto server = galay::tracing::startServerSpan("sampled_parent", sampledParent);
        assert(server.span().spanContext().sampled());
        assert(galay::tracing::currentContext()->sampled());
    }

    {
        auto server = galay::tracing::startServerSpan("unsampled_parent", unsampledParent);
        assert(!server.span().spanContext().sampled());
        assert(!galay::tracing::currentContext()->sampled());
    }

    galay::tracing::clearCurrentContext();
}

void ratioSamplerHandlesBoundaryRatios() {
    galay::tracing::clearCurrentContext();

    {
        galay::tracing::TraceIdRatioSampler none(0.0);
        SamplerScope sampler(&none);
        auto span = galay::tracing::startSpan("ratio_none");
        assert(!span.span().spanContext().sampled());
    }

    {
        galay::tracing::TraceIdRatioSampler all(1.0);
        SamplerScope sampler(&all);
        auto span = galay::tracing::startSpan("ratio_all");
        assert(span.span().spanContext().sampled());
    }

    galay::tracing::clearCurrentContext();
}

void spanGuardIsMoveOnlyAndRestoresOnce() {
    static_assert(!std::is_copy_constructible_v<galay::tracing::SpanGuard>);
    static_assert(!std::is_copy_assignable_v<galay::tracing::SpanGuard>);
    static_assert(std::is_move_constructible_v<galay::tracing::SpanGuard>);
    static_assert(noexcept(std::declval<galay::tracing::SpanGuard&>().~SpanGuard()));

    galay::tracing::clearCurrentContext();

    {
        galay::tracing::SpanGuard moved;
        {
            auto guard = galay::tracing::startSpan("moved");
            const auto active = galay::tracing::currentContext();
            moved = std::move(guard);
            assert(galay::tracing::currentContext() == active);
        }

        assert(galay::tracing::currentContext().has_value());
    }

    assert(!galay::tracing::currentContext().has_value());
}

} // namespace

int main() {
    spanContextRoundTripsTraceIdentity();
    spanStoresLightweightContextAndBuildsPropagationContext();
    spanStoresSemanticFieldsAndBoundsAttributes();
    rootSpanCreatesAndRestoresContext();
    nestedSpanUsesParentTraceAndRestoresParent();
    startServerSpanUsesInboundParentAndRestoresPrevious();
    defaultSpanTimingSkipsTiming();
    enabledSpanTimingRecordsTiming();
    disabledSpanTimingRestoresLowCostBehavior();
    defaultSamplerSamplesRootSpan();
    configuredSamplerCanDropRootSpan();
    parentBasedSamplerInheritsRemoteDecision();
    ratioSamplerHandlesBoundaryRatios();
    spanGuardIsMoveOnlyAndRestoresOnce();
}
