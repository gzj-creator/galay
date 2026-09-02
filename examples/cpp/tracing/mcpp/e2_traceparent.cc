import galay.tracing;

#include <iostream>

int main() {
    auto inbound = galay::tracing::extractTraceparent(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
        "vendor=value");
    if (!inbound.has_value()) {
        return 1;
    }

    auto span = galay::tracing::startServerSpan("GET /orders", *inbound);
    GALAY_LOG_INFO("handling inbound request");

    auto current = galay::tracing::currentContext();
    if (!current.has_value()) {
        return 1;
    }

    std::cout << "traceparent=" << galay::tracing::injectTraceparent(*current) << '\n';
    std::cout << "tracestate=" << galay::tracing::injectTracestate(*current) << '\n';
}
