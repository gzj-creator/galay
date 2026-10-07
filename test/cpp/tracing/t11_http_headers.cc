#include <galay/cpp/galay-tracing/adapters/http_headers.h>

#include <cassert>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::optional<std::string_view> get_header(const std::map<std::string, std::string>& headers, std::string_view name) {
    if (auto it = headers.find(std::string(name)); it != headers.end()) {
        return it->second;
    }
    return std::nullopt;
}

void extracts_inbound_trace_context_from_generic_getter() {
    const std::map<std::string, std::string> headers{
        {"traceparent", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"},
        {"tracestate", "vendor=value"},
    };

    auto context = galay::tracing::extract_trace_context_from_headers([&](std::string_view name) {
        return get_header(headers, name);
    });

    assert(context.has_value());
    assert(context->trace_id() == galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"));
    assert(context->span_id() == galay::tracing::SpanId::from_hex("00f067aa0ba902b7"));
    assert(context->sampled());
    assert(context->tracestate() == "vendor=value");
}

void missing_traceparent_returns_parse_error() {
    const std::map<std::string, std::string> headers{{"tracestate", "vendor=value"}};

    auto context = galay::tracing::extract_trace_context_from_headers([&](std::string_view name) {
        return get_header(headers, name);
    });

    assert(!context.has_value());
    assert(context.error() == galay::tracing::TraceparentError::kMalformed);
}

void injects_outbound_trace_context_through_generic_setter() {
    const auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
    std::map<std::string, std::string> headers;

    const bool injected = galay::tracing::inject_trace_context_to_headers(context, [&](std::string_view name, std::string value) {
        headers[std::string(name)] = std::move(value);
    });

    assert(injected);
    assert(headers["traceparent"] == "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    assert(headers["tracestate"] == "vendor=value");
}

void invalid_context_is_not_injected() {
    std::map<std::string, std::string> headers;

    const bool injected = galay::tracing::inject_trace_context_to_headers(galay::tracing::TraceContext{}, [&](std::string_view name, std::string value) {
        headers[std::string(name)] = std::move(value);
    });

    assert(!injected);
    assert(headers.empty());
}

} // namespace

int main() {
    extracts_inbound_trace_context_from_generic_getter();
    missing_traceparent_returns_parse_error();
    injects_outbound_trace_context_through_generic_setter();
    invalid_context_is_not_injected();
}
