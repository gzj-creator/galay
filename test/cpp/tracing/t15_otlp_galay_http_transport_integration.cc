#include <galay/cpp/galay-tracing/kernel/otlp_http_exporter.h>
#include <galay/cpp/galay-tracing/kernel/span.h>

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <cassert>
#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

galay::tracing::TraceContext make_context() {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01);
}

galay::tracing::Span make_span(std::string_view name) {
    galay::tracing::Span span(std::string(name), make_context());
    span.end();
    return span;
}

galay::kernel::Task<galay::tracing::ExportResult> export_on_scheduler_thread() {
    auto transport = galay::tracing::make_galay_http_otlp_transport();
    galay::tracing::OtlpHttpExporter exporter({}, transport);
    std::vector<galay::tracing::Span> spans;
    spans.push_back(make_span("scheduler-thread"));

    co_return exporter.export_spans(std::span<const galay::tracing::Span>(spans));
}

void rejects_scheduler_thread_blocking() {
    galay::kernel::Runtime runtime = galay::kernel::RuntimeBuilder()
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .build();
    runtime.start();

    auto join = runtime.spawn_io(export_on_scheduler_thread());
    assert(join.has_value());
    auto result = join->join();
    runtime.stop();

    assert(result.has_value());
    assert(result.value() == galay::tracing::ExportResult::kFailure);
}

void rejects_malformed_endpoint() {
    auto transport = galay::tracing::make_galay_http_otlp_transport();
    auto response = transport(galay::tracing::OtlpHttpRequest{
        .endpoint = "ftp://collector.invalid/v1/traces",
        .timeout = std::chrono::milliseconds(10),
        .body = "{}",
    });

    assert(response.status_code == 0);
    assert(!response.error.empty());
}

} // namespace

int main() {
    rejects_scheduler_thread_blocking();
    rejects_malformed_endpoint();
}
