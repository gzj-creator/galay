#include "galay-tracing/context/trace_context.h"
#include "galay-tracing/kernel/otlp_http_exporter.h"
#include "galay-tracing/kernel/span.h"

#include <chrono>
#include <iostream>
#include <span>
#include <string>
#include <vector>

int main() {
    auto config = galay::tracing::OtlpHttpExporterConfig{
        .endpoint = "http://collector.example:4318/v1/traces",
        .timeout = std::chrono::milliseconds(250),
        .headers = {{"authorization", "Bearer token"}},
        .resource_attributes = {
            galay::tracing::spanAttribute("service.name", "order-service"),
            galay::tracing::spanAttribute("deployment.environment", "test"),
        },
        .scope = {
            .name = "order-handler",
            .version = "1.2.3",
        },
    };

    std::size_t requests = 0;
    std::size_t bodyBytes = 0;
    std::string endpoint;

    auto transport = [&](galay::tracing::OtlpHttpRequest request) {
        ++requests;
        endpoint = std::string(request.endpoint);
        bodyBytes = request.body.size();
        if (request.body.find("\"traceId\":\"4bf92f3577b34da6a3ce929d0e0e4736\"") == std::string::npos) {
            return galay::tracing::OtlpHttpResponse{.status_code = 500, .error = "missing trace id"};
        }
        if (request.body.find("\"key\":\"service.name\",\"value\":{\"stringValue\":\"order-service\"}") == std::string::npos) {
            return galay::tracing::OtlpHttpResponse{.status_code = 500, .error = "missing service name"};
        }
        return galay::tracing::OtlpHttpResponse{.status_code = 200};
    };

    galay::tracing::OtlpHttpExporter exporter(config, transport);
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
    context.setParentSpanId(galay::tracing::SpanId::fromHex("1111111111111111"));

    galay::tracing::Span span("GET /orders", context);
    span.end();
    const std::vector spans{span};

    const auto result = exporter.exportSpans(std::span<const galay::tracing::Span>(spans.data(), spans.size()));
    if (result != galay::tracing::ExportResult::kSuccess || requests != 1 || endpoint != config.endpoint) {
        return 1;
    }

    std::cout << "sent " << bodyBytes << " bytes to " << endpoint << '\n';
}
