/**
 * @file traceparent.cc
 * @brief W3C Trace Context traceparent/tracestate 解析与注入实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现 W3C traceparent 头的严格格式校验与解析，以及将 TraceContext
 * 序列化为标准 traceparent 字符串用于出站传播。
 */

#include "traceparent.h"

#include "../common/id_format.h"

#include <array>
#include <cstddef>
#include <string>

namespace galay::tracing {

namespace {

constexpr std::size_t kTraceparentLength = 55;
constexpr char kSeparator = '-';
constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] bool has_expected_separators(std::string_view value) noexcept {
    return value.size() == kTraceparentLength && value[2] == kSeparator && value[35] == kSeparator
        && value[52] == kSeparator;
}

[[nodiscard]] bool parse_flags(std::string_view value, std::uint8_t& flags) noexcept {
    if (value.size() != 2) {
        return false;
    }

    const int high = detail::hex_value(value[0]);
    const int low = detail::hex_value(value[1]);
    if (high < 0 || low < 0) {
        return false;
    }

    flags = static_cast<std::uint8_t>((high << 4) | low);
    return true;
}

} // namespace

std::expected<TraceContext, TraceparentError> extract_traceparent(std::string_view value, std::string_view tracestate) {
    if (!has_expected_separators(value)) {
        return std::unexpected(TraceparentError::kMalformed);
    }

    if (value.substr(0, 2) != "00") {
        return std::unexpected(TraceparentError::kUnsupportedVersion);
    }

    auto traceId = TraceId::from_hex(value.substr(3, TraceId::kHexLength));
    if (!traceId.is_valid()) {
        return std::unexpected(TraceparentError::kInvalidTraceId);
    }

    auto spanId = SpanId::from_hex(value.substr(36, SpanId::kHexLength));
    if (!spanId.is_valid()) {
        return std::unexpected(TraceparentError::kInvalidSpanId);
    }

    std::uint8_t flags = 0;
    if (!parse_flags(value.substr(53, 2), flags)) {
        return std::unexpected(TraceparentError::kInvalidFlags);
    }

    return TraceContext(traceId, spanId, flags, std::string(tracestate));
}

std::string inject_traceparent(const TraceContext& context) {
    if (!context.is_valid()) {
        return {};
    }

    std::array<char, kTraceparentLength> value{};
    value[0] = '0';
    value[1] = '0';
    value[2] = kSeparator;
    if (!context.trace_id().to_hex(value.data() + 3, TraceId::kHexLength)) {
        return {};
    }
    value[35] = kSeparator;
    if (!context.span_id().to_hex(value.data() + 36, SpanId::kHexLength)) {
        return {};
    }
    value[52] = kSeparator;
    value[53] = kHexDigits[context.trace_flags() >> 4U];
    value[54] = kHexDigits[context.trace_flags() & 0x0fU];
    return std::string(value.data(), value.size());
}

std::string inject_tracestate(const TraceContext& context) {
    return context.tracestate();
}

} // namespace galay::tracing
