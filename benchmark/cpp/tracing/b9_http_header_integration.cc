#include <galay/cpp/galay-tracing/adapters/http_headers.h>

#include <galay/cpp/galay-http/protoc/http_header.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::size_t kIterations = 500000;

std::optional<std::string_view> getHeader(const galay::http::HeaderPair& headers, std::string_view name) {
    const auto* value = headers.getValuePtr(std::string(name));
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string_view(*value);
}

void setHeader(galay::http::HeaderPair& headers, std::string_view name, std::string value) {
    static_cast<void>(headers.addHeaderPair(std::string(name), value));
}

galay::tracing::TraceContext makeContext() {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
}

} // namespace

int main() {
    const auto context = makeContext();
    std::size_t propagated = 0;

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kIterations; ++i) {
        galay::http::HeaderPair headers(galay::http::HeaderPair::Mode::ServerSide);
        const bool injected = galay::tracing::injectTraceContextToHeaders(context, [&](std::string_view name, std::string value) {
            setHeader(headers, name, std::move(value));
        });

        auto extracted = galay::tracing::extractTraceContextFromHeaders([&](std::string_view name) {
            return getHeader(headers, name);
        });

        if (injected && extracted.has_value() && extracted->traceId() == context.traceId()) {
            ++propagated;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    std::cout << "iterations=" << kIterations
              << " propagated=" << propagated
              << " ns_total=" << ns
              << " ns_per_op=" << (ns / static_cast<long long>(kIterations))
              << '\n';

    return propagated == kIterations ? 0 : 1;
}
