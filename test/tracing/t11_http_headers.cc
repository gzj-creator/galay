#include <galay/cpp/galay-tracing/adapters/http_headers.h>

#include <cassert>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::optional<std::string_view> getHeader(const std::map<std::string, std::string>& headers, std::string_view name) {
    if (auto it = headers.find(std::string(name)); it != headers.end()) {
        return it->second;
    }
    return std::nullopt;
}

void extractsInboundTraceContextFromGenericGetter() {
    const std::map<std::string, std::string> headers{
        {"traceparent", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"},
        {"tracestate", "vendor=value"},
    };

    auto context = galay::tracing::extractTraceContextFromHeaders([&](std::string_view name) {
        return getHeader(headers, name);
    });

    assert(context.has_value());
    assert(context->traceId() == galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"));
    assert(context->spanId() == galay::tracing::SpanId::fromHex("00f067aa0ba902b7"));
    assert(context->sampled());
    assert(context->tracestate() == "vendor=value");
}

void missingTraceparentReturnsParseError() {
    const std::map<std::string, std::string> headers{{"tracestate", "vendor=value"}};

    auto context = galay::tracing::extractTraceContextFromHeaders([&](std::string_view name) {
        return getHeader(headers, name);
    });

    assert(!context.has_value());
    assert(context.error() == galay::tracing::TraceparentError::kMalformed);
}

void injectsOutboundTraceContextThroughGenericSetter() {
    const auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
    std::map<std::string, std::string> headers;

    const bool injected = galay::tracing::injectTraceContextToHeaders(context, [&](std::string_view name, std::string value) {
        headers[std::string(name)] = std::move(value);
    });

    assert(injected);
    assert(headers["traceparent"] == "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    assert(headers["tracestate"] == "vendor=value");
}

void invalidContextIsNotInjected() {
    std::map<std::string, std::string> headers;

    const bool injected = galay::tracing::injectTraceContextToHeaders(galay::tracing::TraceContext{}, [&](std::string_view name, std::string value) {
        headers[std::string(name)] = std::move(value);
    });

    assert(!injected);
    assert(headers.empty());
}

} // namespace

int main() {
    extractsInboundTraceContextFromGenericGetter();
    missingTraceparentReturnsParseError();
    injectsOutboundTraceContextThroughGenericSetter();
    invalidContextIsNotInjected();
}
