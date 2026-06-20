#include <galay/cpp/galay-tracing/kernel/otlp_http_exporter.h>

#include <cassert>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(GALAY_TRACING_ENABLE_OTLP_HTTP)
#include <galay/cpp/galay-kernel/core/runtime.h>
#endif

namespace {

static_assert(std::is_same_v<
    decltype(std::declval<galay::tracing::OtlpHttpRequest>().method),
    std::string_view>);

static_assert(std::is_same_v<
    decltype(std::declval<galay::tracing::OtlpHttpRequest>().endpoint),
    std::string_view>);

static_assert(std::is_same_v<
    decltype(std::declval<galay::tracing::OtlpHttpRequest>().headers),
    std::span<const galay::tracing::OtlpHttpHeader>>);

galay::tracing::TraceContext makeContext() {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
    context.setParentSpanId(galay::tracing::SpanId::fromHex("1111111111111111"));
    return context;
}

galay::tracing::Span makeSpan(std::string_view name) {
    galay::tracing::Span span(std::string(name), makeContext());
    span.end();
    return span;
}

bool hasHeader(const galay::tracing::OtlpHttpRequest& request, std::string_view name, std::string_view value) {
    for (const auto& header : request.headers) {
        if (header.name == name && header.value == value) {
            return true;
        }
    }
    return false;
}

void configurableEndpointHeadersAndBodyAreSent() {
    auto config = galay::tracing::OtlpHttpExporterConfig{
        .endpoint = "http://collector.example:4318/v1/traces",
        .timeout = std::chrono::milliseconds(250),
        .headers = {{"authorization", "Bearer token"}},
    };

    bool captured = false;
    auto transport = [&](galay::tracing::OtlpHttpRequest request) {
        captured = true;
        assert(request.method == "POST");
        assert(request.endpoint == config.endpoint);
        assert(request.timeout == config.timeout);
        assert(hasHeader(request, "content-type", "application/json"));
        assert(hasHeader(request, "authorization", "Bearer token"));
        assert(request.body.find("\"resourceSpans\"") != std::string::npos);
        assert(request.body.find("\"scopeSpans\"") != std::string::npos);
        assert(request.body.find("\"traceId\":\"4bf92f3577b34da6a3ce929d0e0e4736\"") != std::string::npos);
        assert(request.body.find("\"spanId\":\"00f067aa0ba902b7\"") != std::string::npos);
        assert(request.body.find("\"parentSpanId\":\"1111111111111111\"") != std::string::npos);
        assert(request.body.find("\"traceState\":\"vendor=value\"") != std::string::npos);
        assert(request.body.find("\"name\":\"span \\\"quoted\\\"\"") != std::string::npos);
        return galay::tracing::OtlpHttpResponse{.status_code = 200};
    };

    galay::tracing::OtlpHttpExporter exporter(config, transport);
    const std::vector spans{makeSpan("span \"quoted\"")};

    assert(exporter.exportSpans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);

    assert(captured);
}

void emptyBatchDoesNotSendRequest() {
    bool called = false;
    auto transport = [&](galay::tracing::OtlpHttpRequest) {
        called = true;
        return galay::tracing::OtlpHttpResponse{.status_code = 200};
    };

    galay::tracing::OtlpHttpExporter exporter({}, transport);

    assert(exporter.exportSpans({}) == galay::tracing::ExportResult::kSuccess);
    assert(!called);
}

void nonSuccessStatusFailsExport() {
    auto transport = [](galay::tracing::OtlpHttpRequest) {
        return galay::tracing::OtlpHttpResponse{.status_code = 503, .body = "unavailable"};
    };

    galay::tracing::OtlpHttpExporter exporter({}, transport);
    const std::vector spans{makeSpan("failing")};

    assert(exporter.exportSpans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kFailure);
}

void multipleSpansAreEncodedIntoOneRequest() {
    std::size_t calls = 0;
    auto transport = [&](galay::tracing::OtlpHttpRequest request) {
        ++calls;
        assert(request.body.find("\"spanId\":\"00f067aa0ba902b7\"") != std::string::npos);
        assert(request.body.find("\"spanId\":\"00f067aa0ba902b8\"") != std::string::npos);
        return galay::tracing::OtlpHttpResponse{.status_code = 200};
    };

    galay::tracing::OtlpHttpExporter exporter({}, transport);
    std::vector spans{makeSpan("first"), makeSpan("second")};

    spans[1] = galay::tracing::Span("second", galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b8"),
        0x01));
    spans[1].end();

    assert(exporter.exportSpans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
    assert(calls == 1);
}

void semanticSpanFieldsAreEncoded() {
    bool captured = false;
    auto transport = [&](galay::tracing::OtlpHttpRequest request) {
        captured = true;
        assert(request.body.find("\"kind\":\"SPAN_KIND_CLIENT\"") != std::string::npos);
        assert(request.body.find("\"status\":{\"code\":\"STATUS_CODE_ERROR\",\"message\":\"timeout\"}") != std::string::npos);
        assert(request.body.find("\"attributes\"") != std::string::npos);
        assert(request.body.find("\"key\":\"http.method\",\"value\":{\"stringValue\":\"GET\"}") != std::string::npos);
        assert(request.body.find("\"key\":\"http.status_code\",\"value\":{\"intValue\":\"503\"}") != std::string::npos);
        assert(request.body.find("\"key\":\"retry\",\"value\":{\"boolValue\":true}") != std::string::npos);
        assert(request.body.find("\"key\":\"latency_ms\",\"value\":{\"doubleValue\":12.5}") != std::string::npos);
        return galay::tracing::OtlpHttpResponse{.status_code = 200};
    };

    galay::tracing::Span span("client", makeContext());
    span.setKind(galay::tracing::SpanKind::kClient);
    span.setStatus(galay::tracing::SpanStatusCode::kError, "timeout");
    assert(span.setAttribute("http.method", "GET"));
    assert(span.setAttribute("http.status_code", 503));
    assert(span.setAttribute("retry", true));
    assert(span.setAttribute("latency_ms", 12.5));
    span.end();

    galay::tracing::OtlpHttpExporter exporter({}, transport);
    const std::vector spans{span};

    assert(exporter.exportSpans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
    assert(captured);
}

void resourceAndScopeMetadataAreEncoded() {
    auto config = galay::tracing::OtlpHttpExporterConfig{
        .resource_attributes = {
            galay::tracing::spanAttribute("service.name", "order-service"),
            galay::tracing::spanAttribute("deployment.environment", "test"),
        },
        .scope = {
            .name = "order-handler",
            .version = "1.2.3",
        },
    };

    bool captured = false;
    auto transport = [&](galay::tracing::OtlpHttpRequest request) {
        captured = true;
        assert(request.body.find("\"resource\":{\"attributes\"") != std::string::npos);
        assert(request.body.find("\"key\":\"service.name\",\"value\":{\"stringValue\":\"order-service\"}") != std::string::npos);
        assert(request.body.find("\"key\":\"deployment.environment\",\"value\":{\"stringValue\":\"test\"}") != std::string::npos);
        assert(request.body.find("\"scope\":{\"name\":\"order-handler\",\"version\":\"1.2.3\"}") != std::string::npos);
        return galay::tracing::OtlpHttpResponse{.status_code = 200};
    };

    galay::tracing::OtlpHttpExporter exporter(config, transport);
    const std::vector spans{makeSpan("resource")};

    assert(exporter.exportSpans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
    assert(captured);
}

#if defined(GALAY_TRACING_ENABLE_OTLP_HTTP)
galay::kernel::Task<galay::tracing::ExportResult> exportOnSchedulerThread() {
    auto transport = galay::tracing::makeGalayHttpOtlpTransport();
    galay::tracing::OtlpHttpExporter exporter({}, transport);
    const std::vector spans{makeSpan("scheduler-thread")};

    co_return exporter.exportSpans(std::span<const galay::tracing::Span>(spans));
}

void galayHttpTransportRejectsSchedulerThreadBlocking() {
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

void galayHttpTransportRejectsMalformedEndpoint() {
    auto transport = galay::tracing::makeGalayHttpOtlpTransport();
    auto response = transport(galay::tracing::OtlpHttpRequest{
        .endpoint = "ftp://collector.invalid/v1/traces",
        .timeout = std::chrono::milliseconds(10),
        .body = "{}",
    });

    assert(response.status_code == 0);
    assert(!response.error.empty());
}
#endif

} // namespace

int main() {
    configurableEndpointHeadersAndBodyAreSent();
    emptyBatchDoesNotSendRequest();
    nonSuccessStatusFailsExport();
    multipleSpansAreEncodedIntoOneRequest();
    semanticSpanFieldsAreEncoded();
    resourceAndScopeMetadataAreEncoded();
#if defined(GALAY_TRACING_ENABLE_OTLP_HTTP)
    galayHttpTransportRejectsSchedulerThreadBlocking();
    galayHttpTransportRejectsMalformedEndpoint();
#endif
}
