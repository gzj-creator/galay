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

galay::tracing::TraceContext makeContext() {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01);
}

galay::tracing::Span makeSpan(std::string_view name) {
    galay::tracing::Span span(std::string(name), makeContext());
    span.end();
    return span;
}

galay::kernel::Task<galay::tracing::ExportResult> exportOnSchedulerThread() {
    auto transport = galay::tracing::makeGalayHttpOtlpTransport();
    galay::tracing::OtlpHttpExporter exporter({}, transport);
    const std::vector spans{makeSpan("scheduler-thread")};

    co_return exporter.exportSpans(std::span<const galay::tracing::Span>(spans));
}

void rejectsSchedulerThreadBlocking() {
    galay::kernel::Runtime runtime = galay::kernel::RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    runtime.start();

    auto join = runtime.spawn(exportOnSchedulerThread());
    assert(join.has_value());
    auto result = join->join();
    runtime.stop();

    assert(result.has_value());
    assert(result.value() == galay::tracing::ExportResult::kFailure);
}

void rejectsMalformedEndpoint() {
    auto transport = galay::tracing::makeGalayHttpOtlpTransport();
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
    rejectsSchedulerThreadBlocking();
    rejectsMalformedEndpoint();
}
