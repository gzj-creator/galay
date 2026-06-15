#include "galay-tracing/context/traceparent.h"

#include <cassert>
#include <string>

namespace {

void validTraceparentExtractsContext() {
    const auto context = galay::tracing::extractTraceparent(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
        "vendor=value");

    assert(context.has_value());
    assert(context->traceId().toHex() == "4bf92f3577b34da6a3ce929d0e0e4736");
    assert(context->spanId().toHex() == "00f067aa0ba902b7");
    assert(context->sampled());
    assert(context->traceFlags() == 0x01);
    assert(context->tracestate() == "vendor=value");
}

void injectTraceparentFormatsLowercase() {
    const auto context = galay::tracing::extractTraceparent(
        "00-4BF92F3577B34DA6A3CE929D0E0E4736-00F067AA0BA902B7-01");

    assert(context.has_value());
    assert(galay::tracing::injectTraceparent(*context)
           == "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
}

void rejectsMalformedTraceparent() {
    assert(!galay::tracing::extractTraceparent(
                "00-00000000000000000000000000000000-00f067aa0ba902b7-01")
                .has_value());
    assert(!galay::tracing::extractTraceparent(
                "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01")
                .has_value());
    assert(!galay::tracing::extractTraceparent(
                "01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01")
                .has_value());
    assert(!galay::tracing::extractTraceparent(
                "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-zz")
                .has_value());
    assert(!galay::tracing::extractTraceparent(
                "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-010")
                .has_value());
}

void tracestateRoundTripsAsOpaqueValue() {
    auto context = galay::tracing::extractTraceparent(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00",
        "rojo=00f067aa0ba902b7,congo=t61rcWkgMzE");

    assert(context.has_value());
    assert(!context->sampled());
    assert(galay::tracing::injectTracestate(*context)
           == "rojo=00f067aa0ba902b7,congo=t61rcWkgMzE");
}

} // namespace

int main() {
    validTraceparentExtractsContext();
    injectTraceparentFormatsLowercase();
    rejectsMalformedTraceparent();
    tracestateRoundTripsAsOpaqueValue();
}
