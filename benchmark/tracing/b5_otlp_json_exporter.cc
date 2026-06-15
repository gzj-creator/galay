#include "galay-tracing/kernel/otlp_http_exporter.h"

#include <chrono>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] const char* buildType() {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

galay::tracing::TraceContext makeContext(std::string_view spanHex) {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex(spanHex),
        0x01,
        "vendor=value");
}

std::vector<galay::tracing::Span> makeBatch() {
    constexpr std::string_view kSpanIds[] = {
        "00f067aa0ba902b7",
        "00f067aa0ba902b8",
        "00f067aa0ba902b9",
        "00f067aa0ba902ba",
        "00f067aa0ba902bb",
        "00f067aa0ba902bc",
        "00f067aa0ba902bd",
        "00f067aa0ba902be",
    };

    std::vector<galay::tracing::Span> spans;
    spans.reserve(std::size(kSpanIds));
    for (std::size_t i = 0; i < std::size(kSpanIds); ++i) {
        galay::tracing::Span span("otlp-bench-span", makeContext(kSpanIds[i]));
        span.end();
        spans.push_back(std::move(span));
    }
    return spans;
}

} // namespace

int main() {
    constexpr int kIterations = 10000;
    auto spans = makeBatch();

    std::size_t requests = 0;
    std::size_t bytes = 0;
    galay::tracing::OtlpHttpExporter exporter({}, [&](galay::tracing::OtlpHttpRequest request) {
        ++requests;
        bytes += request.body.size();
        return galay::tracing::OtlpHttpResponse{.status_code = 200};
    });

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        static_cast<void>(exporter.exportSpans(std::span<const galay::tracing::Span>(spans)));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const auto total_spans = static_cast<double>(kIterations) * static_cast<double>(spans.size());

    std::cout << "B5-OtlpJsonExporter workload=" << kIterations
              << " batch_size=" << spans.size()
              << " build=" << buildType()
              << " backend=mock_transport"
              << " ns_per_span=" << (static_cast<double>(ns) / total_spans)
              << " requests=" << requests
              << " avg_body_bytes=" << (bytes / requests) << '\n';
}
