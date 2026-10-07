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

bool equals_ignore_case_ascii(std::string_view lhs, std::string_view rhs) {
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

std::optional<std::string_view> get_header(const galay::http::HeaderPair& headers, std::string_view name) {
    if (const auto* value = headers.get_value_ptr(std::string(name)); value != nullptr) {
        return std::string_view(*value);
    }

    std::optional<std::string_view> found;
    headers.for_each_header([&](std::string_view key, std::string_view value) {
        if (!found.has_value() && equals_ignore_case_ascii(key, name)) {
            found = value;
        }
    });
    return found;
}

void set_header(galay::http::HeaderPair& headers, std::string_view name, std::string value) {
    const auto error = headers.add_header_pair(std::string(name), value);
    assert(error == galay::http::HttpErrorCode::kNoError);
}

galay::tracing::TraceContext make_context() {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01,
        "vendor=value");
}

void extracts_from_server_side_header_pair() {
    galay::http::HeaderPair headers(galay::http::HeaderPair::Mode::ServerSide);
    headers.add_header_pair("TraceParent", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    headers.add_header_pair("TraceState", "vendor=value");

    auto context = galay::tracing::extract_trace_context_from_headers([&](std::string_view name) {
        return get_header(headers, name);
    });

    assert(context.has_value());
    assert(context->trace_id() == galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"));
    assert(context->span_id() == galay::tracing::SpanId::from_hex("00f067aa0ba902b7"));
    assert(context->sampled());
    assert(context->tracestate() == "vendor=value");
}

void injects_into_client_side_header_pair() {
    galay::http::HeaderPair headers(galay::http::HeaderPair::Mode::ClientSide);
    const auto context = make_context();

    const bool injected = galay::tracing::inject_trace_context_to_headers(context, [&](std::string_view name, std::string value) {
        set_header(headers, name, std::move(value));
    });

    assert(injected);
    assert(headers.get_value("Traceparent") == "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    assert(headers.get_value("Tracestate") == "vendor=value");
}

void round_trips_through_http_request_builder_headers() {
    auto request = galay::http::Http1_1RequestBuilder::get("/orders", galay::http::HeaderPair::Mode::ClientSide)
        .host("example.test")
        .build_move();

    const auto context = make_context();
    const bool injected = galay::tracing::inject_trace_context_to_headers(context, [&](std::string_view name, std::string value) {
        set_header(request.header().header_pairs(), name, std::move(value));
    });
    assert(injected);

    auto extracted = galay::tracing::extract_trace_context_from_headers([&](std::string_view name) {
        return get_header(request.header().header_pairs(), name);
    });

    assert(extracted.has_value());
    assert(extracted->trace_id() == context.trace_id());
    assert(extracted->span_id() == context.span_id());
    assert(extracted->tracestate() == context.tracestate());
}

} // namespace

int main() {
    extracts_from_server_side_header_pair();
    injects_into_client_side_header_pair();
    round_trips_through_http_request_builder_headers();
}
