#include <galay/cpp/galay-tracing/context/traceparent.h>

#include <cassert>
#include <string>

namespace {

void valid_traceparent_extracts_context() {
    const auto context = galay::tracing::extract_traceparent(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
        "vendor=value");

    assert(context.has_value());
    assert(context->trace_id().to_hex() == "4bf92f3577b34da6a3ce929d0e0e4736");
    assert(context->span_id().to_hex() == "00f067aa0ba902b7");
    assert(context->sampled());
    assert(context->trace_flags() == 0x01);
    assert(context->tracestate() == "vendor=value");
}

void inject_traceparent_formats_lowercase() {
    const auto context = galay::tracing::extract_traceparent(
        "00-4BF92F3577B34DA6A3CE929D0E0E4736-00F067AA0BA902B7-01");

    assert(context.has_value());
    assert(galay::tracing::inject_traceparent(*context)
           == "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
}

void rejects_malformed_traceparent() {
    assert(!galay::tracing::extract_traceparent(
                "00-00000000000000000000000000000000-00f067aa0ba902b7-01")
                .has_value());
    assert(!galay::tracing::extract_traceparent(
                "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01")
                .has_value());
    assert(!galay::tracing::extract_traceparent(
                "01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01")
                .has_value());
    assert(!galay::tracing::extract_traceparent(
                "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-zz")
                .has_value());
    assert(!galay::tracing::extract_traceparent(
                "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-010")
                .has_value());
}

void tracestate_round_trips_as_opaque_value() {
    auto context = galay::tracing::extract_traceparent(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00",
        "rojo=00f067aa0ba902b7,congo=t61rcWkgMzE");

    assert(context.has_value());
    assert(!context->sampled());
    assert(galay::tracing::inject_tracestate(*context)
           == "rojo=00f067aa0ba902b7,congo=t61rcWkgMzE");
}

} // namespace

int main() {
    valid_traceparent_extracts_context();
    inject_traceparent_formats_lowercase();
    rejects_malformed_traceparent();
    tracestate_round_trips_as_opaque_value();
}
