/**
 * @file http_headers.h
 * @brief Generic HTTP header helpers for W3C trace context propagation.
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details Provides header getter/setter based helpers without depending on a
 * concrete HTTP request or response type. Callers adapt their framework header
 * containers with small lambdas.
 */

#pragma once

#include "../context/trace_context.h"
#include "../context/traceparent.h"

#include <concepts>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace galay::tracing {

/**
 * @brief Header getter concept used by trace context extraction.
 * @details The callable receives a lowercase header name such as "traceparent"
 * and returns an optional string_view that remains valid for the call.
 */
template <typename Getter>
concept TraceHeaderGetter = requires(Getter getter, std::string_view name) {
    { getter(name) } -> std::convertible_to<std::optional<std::string_view>>;
};

/**
 * @brief Header setter concept used by trace context injection.
 * @details The callable receives the lowercase header name and an owning string
 * value. It may copy or move the value into the destination header container.
 */
template <typename Setter>
concept TraceHeaderSetter = requires(Setter setter, std::string_view name, std::string value) {
    setter(name, std::move(value));
};

/**
 * @brief Extract W3C trace context from framework-specific HTTP headers.
 * @param getHeader Callable matching TraceHeaderGetter.
 * @return Parsed TraceContext, or TraceparentError when traceparent is missing
 * or malformed.
 */
template <TraceHeaderGetter Getter>
[[nodiscard]] std::expected<TraceContext, TraceparentError> extractTraceContextFromHeaders(Getter&& getHeader) {
    const auto traceparent = getHeader("traceparent");
    if (!traceparent.has_value()) {
        return std::unexpected(TraceparentError::kMalformed);
    }

    const auto tracestate = getHeader("tracestate").value_or(std::string_view{});
    return extractTraceparent(*traceparent, tracestate);
}

/**
 * @brief Inject W3C trace context into framework-specific HTTP headers.
 * @param context Context to propagate.
 * @param setHeader Callable matching TraceHeaderSetter.
 * @return true when a valid traceparent was written; false for invalid context.
 */
template <TraceHeaderSetter Setter>
[[nodiscard]] bool injectTraceContextToHeaders(const TraceContext& context, Setter&& setHeader) {
    auto traceparent = injectTraceparent(context);
    if (traceparent.empty()) {
        return false;
    }

    setHeader("traceparent", std::move(traceparent));
    auto tracestate = injectTracestate(context);
    if (!tracestate.empty()) {
        setHeader("tracestate", std::move(tracestate));
    }
    return true;
}

} // namespace galay::tracing
