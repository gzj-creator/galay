/**
 * @file traceparent.cc
 * @brief W3C Trace Context traceparent/tracestate 解析与注入实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现 W3C traceparent 头的严格格式校验与解析，以及将 TraceContext
 * 序列化为标准 traceparent 字符串用于出站传播。
 */

#include "galay-tracing/context/traceparent.h"

#include "galay-tracing/common/id_format.h"

#include <array>
#include <cstddef>
#include <string>

namespace galay::tracing {

namespace {

constexpr std::size_t kTraceparentLength = 55;
constexpr char kSeparator = '-';
constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] bool hasExpectedSeparators(std::string_view value) noexcept {
    return value.size() == kTraceparentLength && value[2] == kSeparator && value[35] == kSeparator
        && value[52] == kSeparator;
}

[[nodiscard]] bool parseFlags(std::string_view value, std::uint8_t& flags) noexcept {
    if (value.size() != 2) {
        return false;
    }

    const int high = detail::hexValue(value[0]);
    const int low = detail::hexValue(value[1]);
    if (high < 0 || low < 0) {
        return false;
    }

    flags = static_cast<std::uint8_t>((high << 4) | low);
    return true;
}

} // namespace

std::expected<TraceContext, TraceparentError> extractTraceparent(std::string_view value, std::string_view tracestate) {
    if (!hasExpectedSeparators(value)) {
        return std::unexpected(TraceparentError::kMalformed);
    }

    if (value.substr(0, 2) != "00") {
        return std::unexpected(TraceparentError::kUnsupportedVersion);
    }

    auto traceId = TraceId::fromHex(value.substr(3, TraceId::kHexLength));
    if (!traceId.isValid()) {
        return std::unexpected(TraceparentError::kInvalidTraceId);
    }

    auto spanId = SpanId::fromHex(value.substr(36, SpanId::kHexLength));
    if (!spanId.isValid()) {
        return std::unexpected(TraceparentError::kInvalidSpanId);
    }

    std::uint8_t flags = 0;
    if (!parseFlags(value.substr(53, 2), flags)) {
        return std::unexpected(TraceparentError::kInvalidFlags);
    }

    return TraceContext(traceId, spanId, flags, std::string(tracestate));
}

std::string injectTraceparent(const TraceContext& context) {
    if (!context.isValid()) {
        return {};
    }

    std::string value;
    value.reserve(kTraceparentLength);
    value.append("00-");
    value.append(context.traceId().toHex());
    value.push_back(kSeparator);
    value.append(context.spanId().toHex());
    value.push_back(kSeparator);
    value.push_back(kHexDigits[context.traceFlags() >> 4]);
    value.push_back(kHexDigits[context.traceFlags() & 0x0fU]);
    return value;
}

std::string injectTracestate(const TraceContext& context) {
    return context.tracestate();
}

} // namespace galay::tracing
