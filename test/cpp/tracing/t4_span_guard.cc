#include <galay/cpp/galay-tracing/context/context_storage.h>
#include <galay/cpp/galay-tracing/kernel/sampler.h>
#include <galay/cpp/galay-tracing/kernel/span_guard.h>

#include <cassert>
#include <type_traits>
#include <utility>

namespace {

static_assert(sizeof(galay::tracing::SpanContext) < sizeof(galay::tracing::TraceContext));
static_assert(std::is_same_v<
    decltype(std::declval<const galay::tracing::Span&>().span_context()),
    const galay::tracing::SpanContext&>);
static_assert(std::is_same_v<
    decltype(std::declval<const galay::tracing::Span&>().context()),
    galay::tracing::TraceContext>);

class SpanTimingPolicyScope {
public:
    explicit SpanTimingPolicyScope(galay::tracing::SpanTimingPolicy policy)
        : m_previous(galay::tracing::span_timing_policy()) {
        galay::tracing::set_span_timing_policy(policy);
    }

    ~SpanTimingPolicyScope() {
        galay::tracing::set_span_timing_policy(m_previous);
    }

private:
    galay::tracing::SpanTimingPolicy m_previous;
};

class SamplerScope {
public:
    explicit SamplerScope(const galay::tracing::Sampler* sampler) noexcept {
        galay::tracing::set_sampler(sampler);
    }

    ~SamplerScope() {
        galay::tracing::set_sampler(nullptr);
    }
};

void span_context_round_trips_trace_identity() {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
    const auto parent = galay::tracing::SpanId::from_hex("1111111111111111");
    context.set_parent_span_id(parent);

    const auto spanContext = galay::tracing::SpanContext(context);
    const auto roundTrip = spanContext.to_trace_context(context.tracestate());

    assert(spanContext.is_valid());
    assert(spanContext.trace_id() == context.trace_id());
    assert(spanContext.span_id() == context.span_id());
    assert(spanContext.parent_span_id().has_value());
    assert(*spanContext.parent_span_id() == parent);
    assert(spanContext.sampled());
    assert(roundTrip == context);
}

void span_stores_lightweight_context_and_builds_propagation_context() {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
    const auto parent = galay::tracing::SpanId::from_hex("1111111111111111");
    context.set_parent_span_id(parent);

    const auto spanContext = galay::tracing::SpanContext(context);
    galay::tracing::Span span("fast", spanContext, context.tracestate(), galay::tracing::SpanTimingPolicy::kDisabled);
    const auto propagation = span.context();

    assert(span.span_context() == spanContext);
    assert(propagation == context);
}

void span_stores_semantic_fields_and_bounds_attributes() {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01);
    galay::tracing::Span span("semantic", context);

    assert(span.kind() == galay::tracing::SpanKind::kInternal);
    span.set_kind(galay::tracing::SpanKind::kClient);
    assert(span.kind() == galay::tracing::SpanKind::kClient);

    assert(span.status().code == galay::tracing::SpanStatusCode::kUnset);
    span.set_status(galay::tracing::SpanStatusCode::kError, "timeout");
    assert(span.status().code == galay::tracing::SpanStatusCode::kError);
    assert(span.status().message == "timeout");

    assert(span.set_attribute("http.method", "GET"));
    assert(span.set_attribute("http.status_code", 503));
    assert(span.set_attribute("retry", true));
    assert(span.set_attribute("latency_ms", 12.5));

    const auto attributes = span.attributes();
    assert(attributes.size() == 4);
    assert(attributes[0].name == "http.method");
    assert(attributes[0].value.type() == galay::tracing::SpanAttributeType::kString);
    assert(attributes[0].value.as_string() == "GET");
    assert(attributes[1].value.as_int64() == 503);
    assert(attributes[2].value.as_bool());
    assert(attributes[3].value.as_double() == 12.5);

    for (std::size_t i = attributes.size(); i < galay::tracing::Span::kMaxAttributes; ++i) {
        assert(span.set_attribute("extra", static_cast<std::int64_t>(i)));
    }
    assert(!span.set_attribute("overflow", 1));
    assert(span.attributes().size() == galay::tracing::Span::kMaxAttributes);
}

void root_span_creates_and_restores_context() {
    galay::tracing::clear_current_context();
    galay::tracing::TraceId trace_id;
    galay::tracing::SpanId span_id;

    {
        auto guard = galay::tracing::start_span("root");
        auto current = galay::tracing::current_context();

        assert(current.has_value());
        assert(current->trace_id().is_valid());
        assert(current->span_id().is_valid());
        assert(!current->parent_span_id().has_value());
        assert(guard.span().name() == "root");
        assert(guard.span().kind() == galay::tracing::SpanKind::kInternal);

        trace_id = current->trace_id();
        span_id = current->span_id();
    }

    assert(trace_id.is_valid());
    assert(span_id.is_valid());
    assert(!galay::tracing::current_context().has_value());
}

void nested_span_uses_parent_trace_and_restores_parent() {
    galay::tracing::clear_current_context();

    auto root = galay::tracing::start_span("root");
    const auto rootContext = galay::tracing::current_context();
    assert(rootContext.has_value());

    {
        auto child = galay::tracing::start_span("child");
        const auto childContext = galay::tracing::current_context();

        assert(childContext.has_value());
        assert(childContext->trace_id() == rootContext->trace_id());
        assert(childContext->span_id() != rootContext->span_id());
        assert(childContext->parent_span_id().has_value());
        assert(*childContext->parent_span_id() == rootContext->span_id());
        assert(child.span().name() == "child");
    }

    assert(galay::tracing::current_context() == rootContext);
}

void start_server_span_uses_inbound_parent_and_restores_previous() {
    auto inbound = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
    auto previous = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        galay::tracing::SpanId::from_hex("bbbbbbbbbbbbbbbb"));

    galay::tracing::set_current_context(previous);

    {
        auto server = galay::tracing::start_server_span("server", inbound);
        const auto current = galay::tracing::current_context();

        assert(current.has_value());
        assert(current->trace_id() == inbound.trace_id());
        assert(current->span_id() != inbound.span_id());
        assert(current->parent_span_id().has_value());
        assert(*current->parent_span_id() == inbound.span_id());
        assert(current->sampled());
        assert(current->tracestate() == inbound.tracestate());
        assert(server.span().name() == "server");
        assert(server.span().kind() == galay::tracing::SpanKind::kServer);
    }

    assert(galay::tracing::current_context() == previous);
    galay::tracing::clear_current_context();
}

void default_span_timing_skips_timing() {
    const auto parent = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01);
    galay::tracing::set_current_context(parent);

    auto span = galay::tracing::start_span("default_timing");
    assert(span.span().started_at() == galay::tracing::Span::Clock::time_point{});
    span.end();
    assert(span.span().ended());
    assert(span.span().ended_at() == galay::tracing::Span::Clock::time_point{});

    galay::tracing::clear_current_context();
}

void enabled_span_timing_records_timing() {
    SpanTimingPolicyScope timing{galay::tracing::SpanTimingPolicy::kEnabled};
    const auto parent = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01);
    galay::tracing::set_current_context(parent);

    auto span = galay::tracing::start_span("sampled");
    assert(span.span().started_at() != galay::tracing::Span::Clock::time_point{});
    span.end();
    assert(span.span().ended());
    assert(span.span().ended_at() != galay::tracing::Span::Clock::time_point{});
    assert(span.span().ended_at() >= span.span().started_at());

    galay::tracing::clear_current_context();
}

void disabled_span_timing_restores_low_cost_behavior() {
    SpanTimingPolicyScope timing{galay::tracing::SpanTimingPolicy::kDisabled};
    const auto parent = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01);
    galay::tracing::set_current_context(parent);

    auto span = galay::tracing::start_span("disabled_timing");
    assert(span.span().started_at() == galay::tracing::Span::Clock::time_point{});
    span.end();
    assert(span.span().ended_at() == galay::tracing::Span::Clock::time_point{});

    galay::tracing::clear_current_context();
}

void default_sampler_samples_root_span() {
    SamplerScope sampler(nullptr);
    galay::tracing::clear_current_context();

    auto span = galay::tracing::start_span("root_sampled");

    assert(span.span().span_context().sampled());
    assert(galay::tracing::current_context()->sampled());
    galay::tracing::clear_current_context();
}

void configured_sampler_can_drop_root_span() {
    galay::tracing::AlwaysOffSampler off;
    SamplerScope sampler(&off);
    galay::tracing::clear_current_context();

    auto span = galay::tracing::start_span("root_unsampled");

    assert(!span.span().span_context().sampled());
    assert(!galay::tracing::current_context()->sampled());
    galay::tracing::clear_current_context();
}

void parent_based_sampler_inherits_remote_decision() {
    galay::tracing::AlwaysOffSampler rootOff;
    galay::tracing::ParentBasedSampler parent_based(rootOff);
    SamplerScope sampler(&parent_based);

    auto sampledParent = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01);
    auto unsampledParent = sampledParent;
    unsampledParent.set_trace_flags(0x00);

    {
        auto server = galay::tracing::start_server_span("sampled_parent", sampledParent);
        assert(server.span().span_context().sampled());
        assert(galay::tracing::current_context()->sampled());
    }

    {
        auto server = galay::tracing::start_server_span("unsampled_parent", unsampledParent);
        assert(!server.span().span_context().sampled());
        assert(!galay::tracing::current_context()->sampled());
    }

    galay::tracing::clear_current_context();
}

void ratio_sampler_handles_boundary_ratios() {
    galay::tracing::clear_current_context();

    {
        galay::tracing::TraceIdRatioSampler none(0.0);
        SamplerScope sampler(&none);
        auto span = galay::tracing::start_span("ratio_none");
        assert(!span.span().span_context().sampled());
    }

    {
        galay::tracing::TraceIdRatioSampler all(1.0);
        SamplerScope sampler(&all);
        auto span = galay::tracing::start_span("ratio_all");
        assert(span.span().span_context().sampled());
    }

    galay::tracing::clear_current_context();
}

void ratio_sampler_uses_trace_id_high_bits_threshold() {
    galay::tracing::TraceIdRatioSampler half(0.5);

    const auto belowHalf = galay::tracing::TraceId::from_hex("40000000000000000000000000000000");
    const auto atHalf = galay::tracing::TraceId::from_hex("80000000000000000000000000000000");
    const auto aboveHalf = galay::tracing::TraceId::from_hex("ffffffffffffffff0000000000000000");

    assert(belowHalf.is_valid());
    assert(atHalf.is_valid());
    assert(aboveHalf.is_valid());
    assert(half.should_sample(nullptr, belowHalf));
    assert(!half.should_sample(nullptr, atHalf));
    assert(!half.should_sample(nullptr, aboveHalf));
}

void span_guard_is_move_only_and_restores_once() {
    static_assert(!std::is_copy_constructible_v<galay::tracing::SpanGuard>);
    static_assert(!std::is_copy_assignable_v<galay::tracing::SpanGuard>);
    static_assert(std::is_move_constructible_v<galay::tracing::SpanGuard>);
    static_assert(noexcept(std::declval<galay::tracing::SpanGuard&>().~SpanGuard()));

    galay::tracing::clear_current_context();

    {
        galay::tracing::SpanGuard moved;
        {
            auto guard = galay::tracing::start_span("moved");
            const auto active = galay::tracing::current_context();
            moved = std::move(guard);
            assert(galay::tracing::current_context() == active);
        }

        assert(galay::tracing::current_context().has_value());
    }

    assert(!galay::tracing::current_context().has_value());
}

} // namespace

int main() {
    span_context_round_trips_trace_identity();
    span_stores_lightweight_context_and_builds_propagation_context();
    span_stores_semantic_fields_and_bounds_attributes();
    root_span_creates_and_restores_context();
    nested_span_uses_parent_trace_and_restores_parent();
    start_server_span_uses_inbound_parent_and_restores_previous();
    default_span_timing_skips_timing();
    enabled_span_timing_records_timing();
    disabled_span_timing_restores_low_cost_behavior();
    default_sampler_samples_root_span();
    configured_sampler_can_drop_root_span();
    parent_based_sampler_inherits_remote_decision();
    ratio_sampler_handles_boundary_ratios();
    ratio_sampler_uses_trace_id_high_bits_threshold();
    span_guard_is_move_only_and_restores_once();
}
