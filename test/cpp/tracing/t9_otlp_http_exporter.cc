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

galay::tracing::TraceContext make_context() {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
    context.set_parent_span_id(galay::tracing::SpanId::from_hex("1111111111111111"));
    return context;
}

galay::tracing::Span make_span(std::string_view name) {
    galay::tracing::Span span(std::string(name), make_context());
    span.end();
    return span;
}

bool has_header(const galay::tracing::OtlpHttpRequest& request, std::string_view name, std::string_view value) {
    for (const auto& header : request.headers) {
        if (header.name == name && header.value == value) {
            return true;
        }
    }
    return false;
}

void configurable_endpoint_headers_and_body_are_sent() {
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
        assert(has_header(request, "content-type", "application/json"));
        assert(has_header(request, "authorization", "Bearer token"));
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
    std::vector<galay::tracing::Span> spans;
    spans.push_back(make_span("span \"quoted\""));

    assert(exporter.export_spans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);

    assert(captured);
}

void empty_batch_does_not_send_request() {
    bool called = false;
    auto transport = [&](galay::tracing::OtlpHttpRequest) {
        called = true;
        return galay::tracing::OtlpHttpResponse{.status_code = 200};
    };

    galay::tracing::OtlpHttpExporter exporter({}, transport);

    assert(exporter.export_spans({}) == galay::tracing::ExportResult::kSuccess);
    assert(!called);
}

void non_success_status_fails_export() {
    auto transport = [](galay::tracing::OtlpHttpRequest) {
        return galay::tracing::OtlpHttpResponse{.status_code = 503, .body = "unavailable"};
    };

    galay::tracing::OtlpHttpExporter exporter({}, transport);
    std::vector<galay::tracing::Span> spans;
    spans.push_back(make_span("failing"));

    assert(exporter.export_spans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kFailure);
}

void multiple_spans_are_encoded_into_one_request() {
    std::size_t calls = 0;
    auto transport = [&](galay::tracing::OtlpHttpRequest request) {
        ++calls;
        assert(request.body.find("\"spanId\":\"00f067aa0ba902b7\"") != std::string::npos);
        assert(request.body.find("\"spanId\":\"00f067aa0ba902b8\"") != std::string::npos);
        return galay::tracing::OtlpHttpResponse{.status_code = 200};
    };

    galay::tracing::OtlpHttpExporter exporter({}, transport);
    std::vector<galay::tracing::Span> spans;
    spans.push_back(make_span("first"));
    spans.push_back(make_span("second"));

    spans[1] = galay::tracing::Span("second", galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b8"),
        0x01));
    spans[1].end();

    assert(exporter.export_spans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
    assert(calls == 1);
}

void semantic_span_fields_are_encoded() {
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

    galay::tracing::Span span("client", make_context());
    span.set_kind(galay::tracing::SpanKind::kClient);
    span.set_status(galay::tracing::SpanStatusCode::kError, "timeout");
    assert(span.set_attribute("http.method", "GET"));
    assert(span.set_attribute("http.status_code", 503));
    assert(span.set_attribute("retry", true));
    assert(span.set_attribute("latency_ms", 12.5));
    span.end();

    galay::tracing::OtlpHttpExporter exporter({}, transport);
    std::vector<galay::tracing::Span> spans;
    spans.push_back(std::move(span));

    assert(exporter.export_spans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
    assert(captured);
}

void resource_and_scope_metadata_are_encoded() {
    auto config = galay::tracing::OtlpHttpExporterConfig{
        .resource_attributes = {
            galay::tracing::span_attribute("service.name", "order-service"),
            galay::tracing::span_attribute("deployment.environment", "test"),
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
    std::vector<galay::tracing::Span> spans;
    spans.push_back(make_span("resource"));

    assert(exporter.export_spans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
    assert(captured);
}

} // namespace

int main() {
    configurable_endpoint_headers_and_body_are_sent();
    empty_batch_does_not_send_request();
    non_success_status_fails_export();
    multiple_spans_are_encoded_into_one_request();
    semantic_span_fields_are_encoded();
    resource_and_scope_metadata_are_encoded();
}
