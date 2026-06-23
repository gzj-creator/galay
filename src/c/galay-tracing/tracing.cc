#include <galay/c/galay-tracing/tracing.h>

#include <galay/cpp/galay-tracing/common/span_id.h>
#include <galay/cpp/galay-tracing/common/trace_id.h>
#include <galay/cpp/galay-tracing/context/trace_context.h>
#include <galay/cpp/galay-tracing/context/traceparent.h>
#include <galay/cpp/galay-tracing/kernel/span.h>
#include <galay/cpp/galay-tracing/kernel/tracer_provider.h>

#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

struct galay_tracing_trace_context {
    galay::tracing::TraceContext value;
};

struct galay_tracing_provider {
};

struct galay_tracing_tracer {
    galay_tracing_provider* provider{nullptr};
    std::string name;
};

struct galay_tracing_span {
    galay::tracing::Span value;
    bool ended{false};
};

namespace {

template <std::size_t N>
bool has_non_zero_byte(const uint8_t (&bytes)[N]) noexcept
{
    for (uint8_t byte : bytes) {
        if (byte != 0) {
            return true;
        }
    }
    return false;
}

std::string_view required_view(const char* data, size_t len, bool& ok) noexcept
{
    if (data == nullptr && len != 0) {
        ok = false;
        return {};
    }
    ok = true;
    return std::string_view(data == nullptr ? "" : data, len);
}

galay_status_t copy_string_result(
    const std::string& value,
    char* out,
    size_t out_len,
    size_t* written) noexcept
{
    if (written == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *written = value.size();
    if (value.empty()) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (out == nullptr || out_len < value.size()) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::memcpy(out, value.data(), value.size());
    return GALAY_OK;
}

galay::tracing::TraceId to_cpp_trace_id(const galay_tracing_trace_id_t& id) noexcept
{
    char hex[GALAY_TRACING_TRACE_ID_HEX_LENGTH];
    static constexpr char kHexDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < GALAY_TRACING_TRACE_ID_BYTE_LENGTH; ++i) {
        hex[i * 2] = kHexDigits[id.bytes[i] >> 4U];
        hex[i * 2 + 1] = kHexDigits[id.bytes[i] & 0x0fU];
    }
    return galay::tracing::TraceId::fromHex(std::string_view(hex, sizeof(hex)));
}

galay::tracing::SpanId to_cpp_span_id(const galay_tracing_span_id_t& id) noexcept
{
    char hex[GALAY_TRACING_SPAN_ID_HEX_LENGTH];
    static constexpr char kHexDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < GALAY_TRACING_SPAN_ID_BYTE_LENGTH; ++i) {
        hex[i * 2] = kHexDigits[id.bytes[i] >> 4U];
        hex[i * 2 + 1] = kHexDigits[id.bytes[i] & 0x0fU];
    }
    return galay::tracing::SpanId::fromHex(std::string_view(hex, sizeof(hex)));
}

void copy_trace_id(const galay::tracing::TraceId& source, galay_tracing_trace_id_t* out) noexcept
{
    const auto& bytes = source.bytes();
    for (size_t i = 0; i < bytes.size(); ++i) {
        out->bytes[i] = static_cast<uint8_t>(bytes[i]);
    }
}

void copy_span_id(const galay::tracing::SpanId& source, galay_tracing_span_id_t* out) noexcept
{
    const auto& bytes = source.bytes();
    for (size_t i = 0; i < bytes.size(); ++i) {
        out->bytes[i] = static_cast<uint8_t>(bytes[i]);
    }
}

galay_status_t map_traceparent_error(galay::tracing::TraceparentError error) noexcept
{
    switch (error) {
    case galay::tracing::TraceparentError::kMalformed:
    case galay::tracing::TraceparentError::kInvalidTraceId:
    case galay::tracing::TraceparentError::kInvalidSpanId:
    case galay::tracing::TraceparentError::kInvalidFlags:
        return GALAY_PROTOCOL_ERROR;
    case galay::tracing::TraceparentError::kUnsupportedVersion:
        return GALAY_UNSUPPORTED;
    }
    return GALAY_PROTOCOL_ERROR;
}

galay_status_t validate_attributes(const galay_tracing_attribute_t* attributes, size_t count) noexcept
{
    if (count == 0) {
        return GALAY_OK;
    }
    if (attributes == nullptr || count > galay::tracing::Span::kMaxEventAttributes) {
        return GALAY_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& attr = attributes[i];
        if ((attr.name == nullptr && attr.name_len != 0) || (attr.value == nullptr && attr.value_len != 0)) {
            return GALAY_INVALID_ARGUMENT;
        }
        if (attr.name_len == 0 || attr.value_len > GALAY_TRACING_MAX_ATTRIBUTE_VALUE_LENGTH) {
            return GALAY_INVALID_ARGUMENT;
        }
    }
    return GALAY_OK;
}

std::vector<galay::tracing::SpanAttribute> make_attributes(
    const galay_tracing_attribute_t* attributes,
    size_t count)
{
    std::vector<galay::tracing::SpanAttribute> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& attr = attributes[i];
        result.push_back(galay::tracing::spanAttribute(
            std::string_view(attr.name, attr.name_len),
            std::string_view(attr.value == nullptr ? "" : attr.value, attr.value_len)));
    }
    return result;
}

} // namespace

extern "C" {

galay_status_t galay_tracing_trace_id_generate(galay_tracing_trace_id_t* out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    copy_trace_id(galay::tracing::TraceId::random(), out);
    return GALAY_OK;
}

galay_status_t galay_tracing_trace_id_parse(const char* hex, size_t hex_len, galay_tracing_trace_id_t* out)
{
    bool ok = false;
    const auto view = required_view(hex, hex_len, ok);
    if (!ok || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const auto id = galay::tracing::TraceId::fromHex(view);
    if (!id.isValid()) {
        return GALAY_INVALID_ARGUMENT;
    }
    copy_trace_id(id, out);
    return GALAY_OK;
}

galay_status_t galay_tracing_trace_id_format(
    const galay_tracing_trace_id_t* trace_id,
    char* out,
    size_t out_len,
    size_t* written)
{
    if (written == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *written = GALAY_TRACING_TRACE_ID_HEX_LENGTH;
    if (trace_id == nullptr || out == nullptr || out_len < GALAY_TRACING_TRACE_ID_HEX_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    const auto id = to_cpp_trace_id(*trace_id);
    if (!id.isValid() || !id.toHex(out, out_len)) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_OK;
}

galay_bool_t galay_tracing_trace_id_is_valid(const galay_tracing_trace_id_t* trace_id)
{
    return trace_id != nullptr && has_non_zero_byte(trace_id->bytes) ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_tracing_span_id_generate(galay_tracing_span_id_t* out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    copy_span_id(galay::tracing::SpanId::random(), out);
    return GALAY_OK;
}

galay_status_t galay_tracing_span_id_parse(const char* hex, size_t hex_len, galay_tracing_span_id_t* out)
{
    bool ok = false;
    const auto view = required_view(hex, hex_len, ok);
    if (!ok || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const auto id = galay::tracing::SpanId::fromHex(view);
    if (!id.isValid()) {
        return GALAY_INVALID_ARGUMENT;
    }
    copy_span_id(id, out);
    return GALAY_OK;
}

galay_status_t galay_tracing_span_id_format(
    const galay_tracing_span_id_t* span_id,
    char* out,
    size_t out_len,
    size_t* written)
{
    if (written == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *written = GALAY_TRACING_SPAN_ID_HEX_LENGTH;
    if (span_id == nullptr || out == nullptr || out_len < GALAY_TRACING_SPAN_ID_HEX_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    const auto id = to_cpp_span_id(*span_id);
    if (!id.isValid() || !id.toHex(out, out_len)) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_OK;
}

galay_bool_t galay_tracing_span_id_is_valid(const galay_tracing_span_id_t* span_id)
{
    return span_id != nullptr && has_non_zero_byte(span_id->bytes) ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_tracing_trace_context_create(
    const galay_tracing_trace_id_t* trace_id,
    const galay_tracing_span_id_t* span_id,
    uint8_t trace_flags,
    const char* tracestate,
    size_t tracestate_len,
    galay_tracing_trace_context_t** out)
{
    bool tracestate_ok = false;
    const auto tracestate_view = required_view(tracestate, tracestate_len, tracestate_ok);
    if (trace_id == nullptr || span_id == nullptr || out == nullptr || !tracestate_ok) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    const auto cpp_trace_id = to_cpp_trace_id(*trace_id);
    const auto cpp_span_id = to_cpp_span_id(*span_id);
    if (!cpp_trace_id.isValid() || !cpp_span_id.isValid()) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* handle = new (std::nothrow) galay_tracing_trace_context{
        galay::tracing::TraceContext(cpp_trace_id, cpp_span_id, trace_flags, std::string(tracestate_view))
    };
    if (handle == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    *out = handle;
    return GALAY_OK;
}

void galay_tracing_trace_context_destroy(galay_tracing_trace_context_t** context)
{
    if (context == nullptr || *context == nullptr) {
        return;
    }
    delete *context;
    *context = nullptr;
}

galay_status_t galay_tracing_trace_context_trace_id(
    const galay_tracing_trace_context_t* context,
    galay_tracing_trace_id_t* out)
{
    if (context == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    copy_trace_id(context->value.traceId(), out);
    return GALAY_OK;
}

galay_status_t galay_tracing_trace_context_span_id(
    const galay_tracing_trace_context_t* context,
    galay_tracing_span_id_t* out)
{
    if (context == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    copy_span_id(context->value.spanId(), out);
    return GALAY_OK;
}

galay_status_t galay_tracing_trace_context_flags(const galay_tracing_trace_context_t* context, uint8_t* out)
{
    if (context == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = context->value.traceFlags();
    return GALAY_OK;
}

galay_status_t galay_tracing_traceparent_parse(
    const char* traceparent,
    size_t traceparent_len,
    const char* tracestate,
    size_t tracestate_len,
    galay_tracing_trace_context_t** out)
{
    bool traceparent_ok = false;
    bool tracestate_ok = false;
    const auto traceparent_view = required_view(traceparent, traceparent_len, traceparent_ok);
    const auto tracestate_view = required_view(tracestate, tracestate_len, tracestate_ok);
    if (out == nullptr || !traceparent_ok || !tracestate_ok) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto parsed = galay::tracing::extractTraceparent(traceparent_view, tracestate_view);
    if (!parsed) {
        return map_traceparent_error(parsed.error());
    }
    auto* handle = new (std::nothrow) galay_tracing_trace_context{std::move(*parsed)};
    if (handle == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    *out = handle;
    return GALAY_OK;
}

galay_status_t galay_tracing_traceparent_format(
    const galay_tracing_trace_context_t* context,
    char* out,
    size_t out_len,
    size_t* written)
{
    if (context == nullptr) {
        if (written != nullptr) {
            *written = GALAY_TRACING_TRACEPARENT_LENGTH;
        }
        return GALAY_INVALID_ARGUMENT;
    }
    return copy_string_result(galay::tracing::injectTraceparent(context->value), out, out_len, written);
}

galay_status_t galay_tracing_provider_create(galay_tracing_provider_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    *out = new (std::nothrow) galay_tracing_provider{};
    if (*out == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    return GALAY_OK;
}

void galay_tracing_provider_destroy(galay_tracing_provider_t** provider)
{
    if (provider == nullptr || *provider == nullptr) {
        return;
    }
    delete *provider;
    *provider = nullptr;
}

galay_status_t galay_tracing_tracer_create(
    galay_tracing_provider_t* provider,
    const char* name,
    size_t name_len,
    galay_tracing_tracer_t** out)
{
    bool name_ok = false;
    const auto name_view = required_view(name, name_len, name_ok);
    if (provider == nullptr || out == nullptr || !name_ok || name_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* handle = new (std::nothrow) galay_tracing_tracer{provider, std::string(name_view)};
    if (handle == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    *out = handle;
    return GALAY_OK;
}

void galay_tracing_tracer_destroy(galay_tracing_tracer_t** tracer)
{
    if (tracer == nullptr || *tracer == nullptr) {
        return;
    }
    delete *tracer;
    *tracer = nullptr;
}

galay_status_t galay_tracing_tracer_start_span(
    galay_tracing_tracer_t* tracer,
    const char* name,
    size_t name_len,
    const galay_tracing_trace_context_t* context,
    galay_tracing_span_t** out)
{
    bool name_ok = false;
    const auto name_view = required_view(name, name_len, name_ok);
    if (tracer == nullptr || tracer->provider == nullptr || context == nullptr || out == nullptr || !name_ok
        || name_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* handle = new (std::nothrow) galay_tracing_span{
        galay::tracing::Span(std::string(name_view), context->value),
        false
    };
    if (handle == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    *out = handle;
    return GALAY_OK;
}

galay_status_t galay_tracing_span_add_event(
    galay_tracing_span_t* span,
    const char* name,
    size_t name_len,
    const galay_tracing_attribute_t* attributes,
    size_t attribute_count)
{
    bool name_ok = false;
    const auto name_view = required_view(name, name_len, name_ok);
    if (span == nullptr || !name_ok || name_len == 0 || span->ended) {
        return GALAY_INVALID_ARGUMENT;
    }
    const auto attr_status = validate_attributes(attributes, attribute_count);
    if (attr_status != GALAY_OK) {
        return attr_status;
    }
    if (!span->value.addEvent(name_view, make_attributes(attributes, attribute_count))) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_OK;
}

galay_status_t galay_tracing_span_end(galay_tracing_span_t* span)
{
    if (span == nullptr || span->ended) {
        return GALAY_INVALID_ARGUMENT;
    }
    span->value.end();
    span->ended = true;
    if (auto* processor = galay::tracing::currentSpanProcessor(); processor != nullptr) {
        processor->onEnd(std::move(span->value));
    }
    return GALAY_OK;
}

void galay_tracing_span_destroy(galay_tracing_span_t** span)
{
    if (span == nullptr || *span == nullptr) {
        return;
    }
    delete *span;
    *span = nullptr;
}

} // extern "C"
