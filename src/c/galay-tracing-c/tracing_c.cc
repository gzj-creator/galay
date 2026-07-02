#include <galay/c/galay-tracing-c/tracing_c.h>

#include <galay/cpp/galay-tracing/common/span_id.h>
#include <galay/cpp/galay-tracing/common/trace_id.h>
#include <galay/cpp/galay-tracing/context/trace_context.h>
#include <galay/cpp/galay-tracing/context/traceparent.h>
#include <galay/cpp/galay-tracing/kernel/sampler.h>
#include <galay/cpp/galay-tracing/kernel/span.h>
#include <galay/cpp/galay-tracing/kernel/span_processor.h>
#include <galay/cpp/galay-tracing/log/log_sink.h>
#include <galay/cpp/galay-tracing/log/logger.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using galay::tracing::AlwaysOffSampler;
using galay::tracing::AlwaysOnSampler;
using galay::tracing::LogLevel;
using galay::tracing::LogRecord;
using galay::tracing::LogSink;
using galay::tracing::Logger;
using galay::tracing::Sampler;
using galay::tracing::Span;
using galay::tracing::SpanAttribute;
using galay::tracing::SpanAttributeType;
using galay::tracing::SpanAttributeValue;
using galay::tracing::SpanContext;
using galay::tracing::SpanProcessor;
using galay::tracing::SpanStatusCode;
using galay::tracing::TraceContext;
using galay::tracing::TraceId;
using galay::tracing::TraceIdRatioSampler;
using galay::tracing::SpanId;

struct galay_tracing_trace_context_t {
    TraceContext context;
};

struct galay_tracing_provider_t {
    std::unique_ptr<SpanProcessor> processor;
};

struct galay_tracing_tracer_t {
    galay_tracing_provider_t* provider = nullptr;
    std::string name;
};

struct galay_tracing_span_t {
    galay_tracing_provider_t* provider = nullptr;
    std::unique_ptr<Span> span;
    bool ended = false;
};

struct galay_tracing_sampler_t {
    std::unique_ptr<Sampler> sampler;
};

struct galay_tracing_logger_t {
    std::unique_ptr<Logger> logger;
};

namespace
{

bool valid_span_status(galay_tracing_span_status_code_t code)
{
    return code == GALAY_TRACING_SPAN_STATUS_UNSET ||
        code == GALAY_TRACING_SPAN_STATUS_OK ||
        code == GALAY_TRACING_SPAN_STATUS_ERROR;
}

bool valid_log_level(galay_tracing_log_level_t level)
{
    return level == GALAY_TRACING_LOG_TRACE ||
        level == GALAY_TRACING_LOG_DEBUG ||
        level == GALAY_TRACING_LOG_INFO ||
        level == GALAY_TRACING_LOG_WARN ||
        level == GALAY_TRACING_LOG_ERROR ||
        level == GALAY_TRACING_LOG_OFF;
}

LogLevel to_cpp_log_level(galay_tracing_log_level_t level)
{
    switch (level) {
    case GALAY_TRACING_LOG_TRACE:
        return LogLevel::kTrace;
    case GALAY_TRACING_LOG_DEBUG:
        return LogLevel::kDebug;
    case GALAY_TRACING_LOG_INFO:
        return LogLevel::kInfo;
    case GALAY_TRACING_LOG_WARN:
        return LogLevel::kWarn;
    case GALAY_TRACING_LOG_ERROR:
        return LogLevel::kError;
    case GALAY_TRACING_LOG_OFF:
        return LogLevel::kOff;
    }
    return LogLevel::kOff;
}

SpanStatusCode to_cpp_status(galay_tracing_span_status_code_t code)
{
    switch (code) {
    case GALAY_TRACING_SPAN_STATUS_OK:
        return SpanStatusCode::kOk;
    case GALAY_TRACING_SPAN_STATUS_ERROR:
        return SpanStatusCode::kError;
    case GALAY_TRACING_SPAN_STATUS_UNSET:
        return SpanStatusCode::kUnset;
    }
    return SpanStatusCode::kUnset;
}

galay_tracing_span_status_code_t from_cpp_status(SpanStatusCode code)
{
    switch (code) {
    case SpanStatusCode::kOk:
        return GALAY_TRACING_SPAN_STATUS_OK;
    case SpanStatusCode::kError:
        return GALAY_TRACING_SPAN_STATUS_ERROR;
    case SpanStatusCode::kUnset:
        return GALAY_TRACING_SPAN_STATUS_UNSET;
    }
    return GALAY_TRACING_SPAN_STATUS_UNSET;
}

std::string view_to_string(const char* data, size_t len)
{
    return data == nullptr || len == 0 ? std::string() : std::string(data, len);
}

galay_status_t copy_string(std::string_view value, char* out, size_t out_len, size_t* written)
{
    if (written != nullptr) {
        *written = value.size();
    }
    if (out == nullptr || written == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (out_len < value.size()) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (!value.empty()) {
        std::memcpy(out, value.data(), value.size());
    }
    return GALAY_OK;
}

galay_status_t fill_c_trace_context(const TraceContext& context, galay_tracing_trace_context_t** out)
{
    if (out == nullptr || !context.isValid()) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* result = new (std::nothrow) galay_tracing_trace_context_t();
    if (result == nullptr) {
        *out = nullptr;
        return GALAY_OUT_OF_MEMORY;
    }
    result->context = context;
    *out = result;
    return GALAY_OK;
}

galay_status_t convert_attribute(const galay_tracing_attribute_t& input, SpanAttribute* out)
{
    if (out == nullptr || input.name == nullptr || input.name_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::string name(input.name, input.name_len);
    switch (input.type) {
    case GALAY_TRACING_ATTRIBUTE_STRING:
        if (input.value == nullptr && input.value_len != 0) {
            return GALAY_INVALID_ARGUMENT;
        }
        if (input.value_len > GALAY_TRACING_MAX_ATTRIBUTE_VALUE_LENGTH) {
            return GALAY_INVALID_ARGUMENT;
        }
        *out = SpanAttribute{
            .name = std::move(name),
            .value = SpanAttributeValue::fromString(view_to_string(input.value, input.value_len)),
        };
        return GALAY_OK;
    case GALAY_TRACING_ATTRIBUTE_INT64:
        *out = SpanAttribute{.name = std::move(name), .value = SpanAttributeValue::fromInt64(input.int64_value)};
        return GALAY_OK;
    case GALAY_TRACING_ATTRIBUTE_UINT64:
        *out = SpanAttribute{.name = std::move(name), .value = SpanAttributeValue::fromUInt64(input.uint64_value)};
        return GALAY_OK;
    case GALAY_TRACING_ATTRIBUTE_DOUBLE:
        *out = SpanAttribute{.name = std::move(name), .value = SpanAttributeValue::fromDouble(input.double_value)};
        return GALAY_OK;
    case GALAY_TRACING_ATTRIBUTE_BOOL:
        *out = SpanAttribute{
            .name = std::move(name),
            .value = SpanAttributeValue::fromBool(input.bool_value == GALAY_TRUE),
        };
        return GALAY_OK;
    }
    return GALAY_INVALID_ARGUMENT;
}

galay_status_t convert_attributes(const galay_tracing_attribute_t* attributes,
                                  size_t attribute_count,
                                  std::vector<SpanAttribute>* out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    out->clear();
    if (attribute_count == 0) {
        return GALAY_OK;
    }
    if (attributes == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    out->reserve(attribute_count);
    for (size_t i = 0; i < attribute_count; ++i) {
        SpanAttribute attribute;
        const galay_status_t converted = convert_attribute(attributes[i], &attribute);
        if (converted != GALAY_OK) {
            return converted;
        }
        out->push_back(std::move(attribute));
    }
    return GALAY_OK;
}

void append_json_string(std::string& out, std::string_view value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (ch < 0x20) {
                out.append("\\u00");
                out.push_back(kHex[ch >> 4U]);
                out.push_back(kHex[ch & 0x0FU]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

void append_attribute_value(std::string& out, const SpanAttributeValue& value)
{
    switch (value.type()) {
    case SpanAttributeType::kInt64:
        out.append(std::to_string(value.asInt64()));
        break;
    case SpanAttributeType::kUInt64:
        out.append(std::to_string(value.asUInt64()));
        break;
    case SpanAttributeType::kDouble:
        out.append(std::to_string(value.asDouble()));
        break;
    case SpanAttributeType::kBool:
        out.append(value.asBool() ? "true" : "false");
        break;
    case SpanAttributeType::kString:
        append_json_string(out, value.asString());
        break;
    }
}

void append_attributes(std::string& out, std::span<const SpanAttribute> attributes)
{
    out.push_back('[');
    for (size_t i = 0; i < attributes.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out.append("{\"key\":");
        append_json_string(out, attributes[i].name);
        out.append(",\"value\":");
        append_attribute_value(out, attributes[i].value);
        out.push_back('}');
    }
    out.push_back(']');
}

std::string render_span_json(const Span& span)
{
    std::string line;
    const SpanContext& context = span.spanContext();
    line.append("{\"name\":");
    append_json_string(line, span.name());
    line.append(",\"trace_id\":\"");
    line.append(context.traceId().toHex());
    line.append("\",\"span_id\":\"");
    line.append(context.spanId().toHex());
    line.append("\",\"sampled\":");
    line.append(context.sampled() ? "true" : "false");
    line.append(",\"status\":{\"code\":");
    append_json_string(line, span.status().code == SpanStatusCode::kError
        ? "error"
        : (span.status().code == SpanStatusCode::kOk ? "ok" : "unset"));
    line.append(",\"message\":");
    append_json_string(line, span.status().message);
    line.append("},\"attributes\":");
    append_attributes(line, span.attributes());
    line.append(",\"events\":[");
    for (size_t i = 0; i < span.events().size(); ++i) {
        if (i != 0) {
            line.push_back(',');
        }
        line.append("{\"name\":");
        append_json_string(line, span.events()[i].name);
        line.append(",\"attributes\":");
        append_attributes(line, span.events()[i].attributes);
        line.push_back('}');
    }
    line.append("],\"links\":[");
    for (size_t i = 0; i < span.links().size(); ++i) {
        if (i != 0) {
            line.push_back(',');
        }
        line.append("{\"trace_id\":\"");
        line.append(span.links()[i].context.traceId().toHex());
        line.append("\",\"span_id\":\"");
        line.append(span.links()[i].context.spanId().toHex());
        line.append("\",\"attributes\":");
        append_attributes(line, span.links()[i].attributes);
        line.push_back('}');
    }
    line.append("]}");
    return line;
}

class SyncFileSpanProcessor final : public SpanProcessor {
public:
    explicit SyncFileSpanProcessor(std::string path)
        : m_out(path, std::ios::out | std::ios::app) {}

    void onEnd(Span&& span) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown || !m_out) {
            return;
        }
        m_out << render_span_json(span) << '\n';
    }

    bool forceFlush(std::chrono::milliseconds) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_out) {
            return false;
        }
        m_out.flush();
        return static_cast<bool>(m_out);
    }

    bool shutdown(std::chrono::milliseconds) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_shutdown) {
            m_out.flush();
            m_out.close();
            m_shutdown = true;
        }
        return true;
    }

private:
    std::mutex m_mutex;
    std::ofstream m_out;
    bool m_shutdown = false;
};

class FileLogSink final : public LogSink {
public:
    explicit FileLogSink(std::string path)
        : m_out(path, std::ios::out | std::ios::app) {}

    void write(const LogRecord& record) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_out) {
            return;
        }
        m_out << record.message;
        if (record.context.has_value() && record.context->isValid()) {
            m_out << " trace_id=" << record.context->traceId().toHex()
                  << " span_id=" << record.context->spanId().toHex();
        }
        m_out << '\n';
        m_out.flush();
    }

private:
    std::mutex m_mutex;
    std::ofstream m_out;
};

std::chrono::milliseconds timeout_from_ms(int64_t timeout_ms)
{
    if (timeout_ms < 0) {
        return std::chrono::milliseconds::max();
    }
    return std::chrono::milliseconds(timeout_ms);
}

} // namespace

extern "C" {

const char* galay_tracing_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_tracing_trace_id_generate(galay_tracing_trace_id_t* out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const auto id = TraceId::random();
    const auto& bytes = id.bytes();
    for (size_t i = 0; i < bytes.size(); ++i) {
        out->bytes[i] = static_cast<uint8_t>(bytes[i]);
    }
    return GALAY_OK;
}

galay_bool_t galay_tracing_trace_id_is_valid(const galay_tracing_trace_id_t* id)
{
    if (id == nullptr) {
        return GALAY_FALSE;
    }
    char hex[GALAY_TRACING_TRACE_ID_HEX_LENGTH];
    size_t written = 0;
    if (galay_tracing_trace_id_format(id, hex, sizeof(hex), &written) != GALAY_OK) {
        return GALAY_FALSE;
    }
    return TraceId::fromHex(std::string_view(hex, written)).isValid() ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_tracing_trace_id_format(const galay_tracing_trace_id_t* id, char* out,
                                             size_t out_len, size_t* written)
{
    if (written != nullptr) {
        *written = GALAY_TRACING_TRACE_ID_HEX_LENGTH;
    }
    if (id == nullptr || out == nullptr || written == nullptr || out_len < GALAY_TRACING_TRACE_ID_HEX_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(id->bytes); ++i) {
        out[i * 2] = kHex[(id->bytes[i] >> 4U) & 0x0FU];
        out[i * 2 + 1] = kHex[id->bytes[i] & 0x0FU];
    }
    return GALAY_OK;
}

galay_status_t galay_tracing_trace_id_parse(const char* data, size_t data_len,
                                            galay_tracing_trace_id_t* out)
{
    if (data == nullptr || out == nullptr || data_len != GALAY_TRACING_TRACE_ID_HEX_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    const TraceId id = TraceId::fromHex(std::string_view(data, data_len));
    if (!id.isValid()) {
        return GALAY_INVALID_ARGUMENT;
    }
    const auto& bytes = id.bytes();
    for (size_t i = 0; i < bytes.size(); ++i) {
        out->bytes[i] = static_cast<uint8_t>(bytes[i]);
    }
    return GALAY_OK;
}

galay_status_t galay_tracing_span_id_generate(galay_tracing_span_id_t* out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const auto id = SpanId::random();
    const auto& bytes = id.bytes();
    for (size_t i = 0; i < bytes.size(); ++i) {
        out->bytes[i] = static_cast<uint8_t>(bytes[i]);
    }
    return GALAY_OK;
}

galay_bool_t galay_tracing_span_id_is_valid(const galay_tracing_span_id_t* id)
{
    if (id == nullptr) {
        return GALAY_FALSE;
    }
    char hex[GALAY_TRACING_SPAN_ID_HEX_LENGTH];
    size_t written = 0;
    if (galay_tracing_span_id_format(id, hex, sizeof(hex), &written) != GALAY_OK) {
        return GALAY_FALSE;
    }
    return SpanId::fromHex(std::string_view(hex, written)).isValid() ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_tracing_span_id_format(const galay_tracing_span_id_t* id, char* out,
                                            size_t out_len, size_t* written)
{
    if (written != nullptr) {
        *written = GALAY_TRACING_SPAN_ID_HEX_LENGTH;
    }
    if (id == nullptr || out == nullptr || written == nullptr || out_len < GALAY_TRACING_SPAN_ID_HEX_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(id->bytes); ++i) {
        out[i * 2] = kHex[(id->bytes[i] >> 4U) & 0x0FU];
        out[i * 2 + 1] = kHex[id->bytes[i] & 0x0FU];
    }
    return GALAY_OK;
}

galay_status_t galay_tracing_span_id_parse(const char* data, size_t data_len,
                                           galay_tracing_span_id_t* out)
{
    if (data == nullptr || out == nullptr || data_len != GALAY_TRACING_SPAN_ID_HEX_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    const SpanId id = SpanId::fromHex(std::string_view(data, data_len));
    if (!id.isValid()) {
        return GALAY_INVALID_ARGUMENT;
    }
    const auto& bytes = id.bytes();
    for (size_t i = 0; i < bytes.size(); ++i) {
        out->bytes[i] = static_cast<uint8_t>(bytes[i]);
    }
    return GALAY_OK;
}

galay_status_t galay_tracing_traceparent_parse(const char* data, size_t data_len,
                                               const char* tracestate, size_t tracestate_len,
                                               galay_tracing_trace_context_t** out)
{
    return galay_tracing_trace_context_extract(data, data_len, tracestate, tracestate_len, out);
}

galay_status_t galay_tracing_traceparent_format(const galay_tracing_trace_context_t* context,
                                                char* out, size_t out_len, size_t* written)
{
    if (context == nullptr || !context->context.isValid()) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::string value = galay::tracing::injectTraceparent(context->context);
    return copy_string(value, out, out_len, written);
}

galay_status_t galay_tracing_trace_context_extract(const char* traceparent, size_t traceparent_len,
                                                   const char* tracestate, size_t tracestate_len,
                                                   galay_tracing_trace_context_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (traceparent == nullptr || out == nullptr) {
        return GALAY_PROTOCOL_ERROR;
    }
    const auto extracted = galay::tracing::extractTraceparent(
        std::string_view(traceparent, traceparent_len),
        tracestate == nullptr ? std::string_view{} : std::string_view(tracestate, tracestate_len));
    if (!extracted) {
        return GALAY_PROTOCOL_ERROR;
    }
    return fill_c_trace_context(*extracted, out);
}

galay_status_t galay_tracing_trace_context_inject(const galay_tracing_trace_context_t* context,
                                                  char* traceparent_out, size_t traceparent_out_len,
                                                  size_t* traceparent_written,
                                                  char* tracestate_out, size_t tracestate_out_len,
                                                  size_t* tracestate_written)
{
    if (context == nullptr || !context->context.isValid()) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::string traceparent = galay::tracing::injectTraceparent(context->context);
    const std::string tracestate = galay::tracing::injectTracestate(context->context);
    const galay_status_t traceparent_status =
        copy_string(traceparent, traceparent_out, traceparent_out_len, traceparent_written);
    if (traceparent_status != GALAY_OK) {
        return traceparent_status;
    }
    return copy_string(tracestate, tracestate_out, tracestate_out_len, tracestate_written);
}

void galay_tracing_trace_context_destroy(galay_tracing_trace_context_t** context)
{
    if (context == nullptr || *context == nullptr) {
        return;
    }
    delete *context;
    *context = nullptr;
}

galay_status_t galay_tracing_trace_context_flags(const galay_tracing_trace_context_t* context,
                                                 uint8_t* flags)
{
    if (context == nullptr || flags == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *flags = context->context.traceFlags();
    return GALAY_OK;
}

galay_status_t galay_tracing_provider_create(galay_tracing_provider_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_tracing_provider_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_tracing_provider_destroy(galay_tracing_provider_t** provider)
{
    if (provider == nullptr || *provider == nullptr) {
        return;
    }
    delete *provider;
    *provider = nullptr;
}

galay_status_t galay_tracing_provider_set_file_exporter(galay_tracing_provider_t* provider,
                                                        const char* path, size_t path_len)
{
    if (provider == nullptr || path == nullptr || path_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* processor = new (std::nothrow) SyncFileSpanProcessor(std::string(path, path_len));
    if (processor == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    provider->processor.reset(processor);
    return GALAY_OK;
}

galay_status_t galay_tracing_provider_force_flush(galay_tracing_provider_t* provider,
                                                  int64_t timeout_ms)
{
    if (provider == nullptr || provider->processor == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return provider->processor->forceFlush(timeout_from_ms(timeout_ms)) ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t galay_tracing_provider_shutdown(galay_tracing_provider_t* provider,
                                               int64_t timeout_ms)
{
    if (provider == nullptr || provider->processor == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return provider->processor->shutdown(timeout_from_ms(timeout_ms)) ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t galay_tracing_tracer_create(galay_tracing_provider_t* provider, const char* name,
                                           size_t name_len, galay_tracing_tracer_t** out)
{
    if (provider == nullptr || name == nullptr || name_len == 0 || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* tracer = new (std::nothrow) galay_tracing_tracer_t();
    if (tracer == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    tracer->provider = provider;
    tracer->name.assign(name, name_len);
    *out = tracer;
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

galay_status_t galay_tracing_tracer_start_span(galay_tracing_tracer_t* tracer, const char* name,
                                               size_t name_len,
                                               const galay_tracing_trace_context_t* context,
                                               galay_tracing_span_t** out)
{
    if (tracer == nullptr || name == nullptr || name_len == 0 || context == nullptr ||
        !context->context.isValid() || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;

    TraceContext child_context(
        context->context.traceId(),
        SpanId::random(),
        context->context.traceFlags(),
        context->context.tracestate());
    child_context.setParentSpanId(context->context.spanId());

    auto* span = new (std::nothrow) galay_tracing_span_t();
    if (span == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    span->provider = tracer->provider;
    span->span.reset(new (std::nothrow) Span(std::string(name, name_len), child_context));
    if (span->span == nullptr) {
        delete span;
        return GALAY_OUT_OF_MEMORY;
    }
    *out = span;
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

galay_status_t galay_tracing_span_add_event(galay_tracing_span_t* span, const char* name,
                                            size_t name_len,
                                            const galay_tracing_attribute_t* attributes,
                                            size_t attribute_count)
{
    if (span == nullptr || span->span == nullptr || span->ended || name == nullptr || name_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::vector<SpanAttribute> converted;
    const galay_status_t status = convert_attributes(attributes, attribute_count, &converted);
    if (status != GALAY_OK) {
        return status;
    }
    return span->span->addEvent(std::string_view(name, name_len), std::move(converted))
        ? GALAY_OK
        : GALAY_INTERNAL_ERROR;
}

galay_status_t galay_tracing_span_set_attribute(galay_tracing_span_t* span,
                                                const galay_tracing_attribute_t* attribute)
{
    if (span == nullptr || span->span == nullptr || span->ended || attribute == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    SpanAttribute converted;
    const galay_status_t status = convert_attribute(*attribute, &converted);
    if (status != GALAY_OK) {
        return status;
    }
    return span->span->setAttribute(std::move(converted)) ? GALAY_OK : GALAY_INTERNAL_ERROR;
}

galay_status_t galay_tracing_span_set_status(galay_tracing_span_t* span,
                                             galay_tracing_span_status_code_t code,
                                             const char* message, size_t message_len)
{
    if (span == nullptr || span->span == nullptr || span->ended || !valid_span_status(code) ||
        (message == nullptr && message_len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    span->span->setStatus(to_cpp_status(code), view_to_string(message, message_len));
    return GALAY_OK;
}

galay_status_t galay_tracing_span_add_link(galay_tracing_span_t* span,
                                           const galay_tracing_trace_context_t* context,
                                           const galay_tracing_attribute_t* attributes,
                                           size_t attribute_count)
{
    if (span == nullptr || span->span == nullptr || span->ended || context == nullptr ||
        !context->context.isValid()) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::vector<SpanAttribute> converted;
    const galay_status_t status = convert_attributes(attributes, attribute_count, &converted);
    if (status != GALAY_OK) {
        return status;
    }
    return span->span->addLink(SpanContext(context->context),
                               context->context.tracestate(),
                               std::move(converted))
        ? GALAY_OK
        : GALAY_INTERNAL_ERROR;
}

galay_status_t galay_tracing_span_attribute_count(const galay_tracing_span_t* span, size_t* out)
{
    if (span == nullptr || span->span == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = span->span->attributes().size();
    return GALAY_OK;
}

galay_status_t galay_tracing_span_event_count(const galay_tracing_span_t* span, size_t* out)
{
    if (span == nullptr || span->span == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = span->span->events().size();
    return GALAY_OK;
}

galay_status_t galay_tracing_span_link_count(const galay_tracing_span_t* span, size_t* out)
{
    if (span == nullptr || span->span == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = span->span->links().size();
    return GALAY_OK;
}

galay_status_t galay_tracing_span_status(const galay_tracing_span_t* span,
                                         galay_tracing_span_status_code_t* out)
{
    if (span == nullptr || span->span == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = from_cpp_status(span->span->status().code);
    return GALAY_OK;
}

galay_status_t galay_tracing_span_end(galay_tracing_span_t* span)
{
    if (span == nullptr || span->span == nullptr || span->ended) {
        return GALAY_INVALID_ARGUMENT;
    }
    span->span->end();
    span->ended = true;
    if (span->provider != nullptr && span->provider->processor != nullptr) {
        span->provider->processor->onEnd(std::move(*span->span));
    }
    return GALAY_OK;
}

galay_status_t galay_tracing_sampler_create(galay_tracing_sampler_kind_t kind, double ratio,
                                            galay_tracing_sampler_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* sampler = new (std::nothrow) galay_tracing_sampler_t();
    if (sampler == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    switch (kind) {
    case GALAY_TRACING_SAMPLER_ALWAYS_ON:
        sampler->sampler.reset(new (std::nothrow) AlwaysOnSampler());
        break;
    case GALAY_TRACING_SAMPLER_ALWAYS_OFF:
        sampler->sampler.reset(new (std::nothrow) AlwaysOffSampler());
        break;
    case GALAY_TRACING_SAMPLER_TRACE_ID_RATIO:
        if (ratio < 0.0 || ratio > 1.0) {
            delete sampler;
            return GALAY_INVALID_ARGUMENT;
        }
        sampler->sampler.reset(new (std::nothrow) TraceIdRatioSampler(ratio));
        break;
    }
    if (sampler->sampler == nullptr) {
        delete sampler;
        return GALAY_OUT_OF_MEMORY;
    }
    *out = sampler;
    return GALAY_OK;
}

void galay_tracing_sampler_destroy(galay_tracing_sampler_t** sampler)
{
    if (sampler == nullptr || *sampler == nullptr) {
        return;
    }
    delete *sampler;
    *sampler = nullptr;
}

galay_status_t galay_tracing_sampler_should_sample(const galay_tracing_sampler_t* sampler,
                                                   const galay_tracing_trace_context_t* context,
                                                   galay_bool_t* out)
{
    if (sampler == nullptr || sampler->sampler == nullptr || context == nullptr ||
        !context->context.isValid() || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const SpanContext parent(context->context);
    *out = sampler->sampler->shouldSample(&parent, context->context.traceId())
        ? GALAY_TRUE
        : GALAY_FALSE;
    return GALAY_OK;
}

galay_status_t galay_tracing_logger_create_file(const char* path, size_t path_len,
                                                galay_tracing_log_level_t level,
                                                galay_tracing_logger_t** out)
{
    if (path == nullptr || path_len == 0 || out == nullptr || !valid_log_level(level)) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* logger = new (std::nothrow) galay_tracing_logger_t();
    if (logger == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    logger->logger.reset(new (std::nothrow) Logger(to_cpp_log_level(level)));
    if (logger->logger == nullptr) {
        delete logger;
        return GALAY_OUT_OF_MEMORY;
    }
    std::shared_ptr<LogSink> sink(new (std::nothrow) FileLogSink(std::string(path, path_len)));
    if (sink == nullptr) {
        delete logger;
        return GALAY_OUT_OF_MEMORY;
    }
    logger->logger->addSink(std::move(sink));
    *out = logger;
    return GALAY_OK;
}

galay_status_t galay_tracing_logger_log(galay_tracing_logger_t* logger,
                                        galay_tracing_log_level_t level,
                                        const galay_tracing_trace_context_t* context,
                                        const char* message, size_t message_len)
{
    if (logger == nullptr || logger->logger == nullptr || !valid_log_level(level) ||
        message == nullptr || message_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (!logger->logger->isEnabled(to_cpp_log_level(level))) {
        return GALAY_OK;
    }
    std::optional<TraceContext> trace_context;
    if (context != nullptr && context->context.isValid()) {
        trace_context = context->context;
    }
    logger->logger->write(LogRecord{
        .level = to_cpp_log_level(level),
        .message = std::string(message, message_len),
        .source = {},
        .context = galay::tracing::makeLogContext(trace_context),
    });
    return GALAY_OK;
}

void galay_tracing_logger_destroy(galay_tracing_logger_t** logger)
{
    if (logger == nullptr || *logger == nullptr) {
        return;
    }
    delete *logger;
    *logger = nullptr;
}

}
