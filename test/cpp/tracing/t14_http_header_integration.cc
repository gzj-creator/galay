#include <galay/cpp/galay-tracing/adapters/http_headers.h>

#include <galay/cpp/galay-http/builder/http_builder.h>
#include <galay/cpp/galay-http/protoc/http_header.h>

#include <cassert>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

bool equalsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

std::optional<std::string_view> getHeader(const galay::http::HeaderPair& headers, std::string_view name) {
    if (const auto* value = headers.getValuePtr(std::string(name)); value != nullptr) {
        return std::string_view(*value);
    }

    std::optional<std::string_view> found;
    headers.forEachHeader([&](std::string_view key, std::string_view value) {
        if (!found.has_value() && equalsIgnoreCaseAscii(key, name)) {
            found = value;
        }
    });
    return found;
}

void setHeader(galay::http::HeaderPair& headers, std::string_view name, std::string value) {
    const auto error = headers.addHeaderPair(std::string(name), value);
    assert(error == galay::http::HttpErrorCode::kNoError);
}

galay::tracing::TraceContext makeContext() {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
}

void extractsFromServerSideHeaderPair() {
    galay::http::HeaderPair headers(galay::http::HeaderPair::Mode::ServerSide);
    headers.addHeaderPair("TraceParent", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    headers.addHeaderPair("TraceState", "vendor=value");

    auto context = galay::tracing::extractTraceContextFromHeaders([&](std::string_view name) {
        return getHeader(headers, name);
    });

    assert(context.has_value());
    assert(context->traceId() == galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"));
    assert(context->spanId() == galay::tracing::SpanId::fromHex("00f067aa0ba902b7"));
    assert(context->sampled());
    assert(context->tracestate() == "vendor=value");
}

void injectsIntoClientSideHeaderPair() {
    galay::http::HeaderPair headers(galay::http::HeaderPair::Mode::ClientSide);
    const auto context = makeContext();

    const bool injected = galay::tracing::injectTraceContextToHeaders(context, [&](std::string_view name, std::string value) {
        setHeader(headers, name, std::move(value));
    });

    assert(injected);
    assert(headers.getValue("Traceparent") == "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    assert(headers.getValue("Tracestate") == "vendor=value");
}

void roundTripsThroughHttpRequestBuilderHeaders() {
    auto request = galay::http::Http1_1RequestBuilder::get("/orders", galay::http::HeaderPair::Mode::ClientSide)
        .host("example.test")
        .buildMove();

    const auto context = makeContext();
    const bool injected = galay::tracing::injectTraceContextToHeaders(context, [&](std::string_view name, std::string value) {
        setHeader(request.header().headerPairs(), name, std::move(value));
    });
    assert(injected);

    auto extracted = galay::tracing::extractTraceContextFromHeaders([&](std::string_view name) {
        return getHeader(request.header().headerPairs(), name);
    });

    assert(extracted.has_value());
    assert(extracted->traceId() == context.traceId());
    assert(extracted->spanId() == context.spanId());
    assert(extracted->tracestate() == context.tracestate());
}

} // namespace

int main() {
    extractsFromServerSideHeaderPair();
    injectsIntoClientSideHeaderPair();
    roundTripsThroughHttpRequestBuilderHeaders();
}
