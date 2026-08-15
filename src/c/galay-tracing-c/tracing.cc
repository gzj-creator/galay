#include <galay/c/galay-tracing-c/tracing.h>

#include <openssl/rand.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct TraceContextData {
    galay_tracing_trace_id_t trace_id{};
    galay_tracing_span_id_t span_id{};
    galay_tracing_span_id_t parent_span_id{};
    std::string tracestate;
    uint8_t flags = 0;
};

struct TraceAttribute {
    std::string name;
    std::string string_value;
    galay_tracing_attribute_type_t type = GALAY_TRACING_ATTRIBUTE_STRING;
    int64_t int64_value = 0;
    uint64_t uint64_value = 0;
    double double_value = 0.0;
    bool bool_value = false;
};

struct TraceEvent {
    std::string name;
    std::vector<TraceAttribute> attributes;
};

struct TraceLink {
    TraceContextData context;
    std::vector<TraceAttribute> attributes;
};

struct galay_tracing_trace_context_t {
    TraceContextData context;
};

struct galay_tracing_provider_t {
    std::ofstream output;
    std::mutex mutex;
    bool configured = false;
    bool shutdown = false;
};

struct galay_tracing_tracer_t {
    galay_tracing_provider_t* provider = nullptr;
    std::string name;
};

struct galay_tracing_span_t {
    galay_tracing_provider_t* provider = nullptr;
    TraceContextData context;
    std::string name;
    std::string status_message;
    std::vector<TraceAttribute> attributes;
    std::vector<TraceEvent> events;
    std::vector<TraceLink> links;
    galay_tracing_span_status_code_t status = GALAY_TRACING_SPAN_STATUS_UNSET;
    bool ended = false;
};

struct galay_tracing_sampler_t {
    galay_tracing_sampler_kind_t kind = GALAY_TRACING_SAMPLER_ALWAYS_OFF;
    double ratio = 0.0;
};

struct galay_tracing_logger_t {
    std::ofstream output;
    std::mutex mutex;
    galay_tracing_log_level_t level = GALAY_TRACING_LOG_OFF;
};

bool bytes_valid(const uint8_t* data, size_t size)
{
    if (data == nullptr) return false;
    for (size_t i = 0; i < size; ++i) {
        if (data[i] != 0) return true;
    }
    return false;
}

bool context_valid(const TraceContextData& context)
{
    return bytes_valid(context.trace_id.bytes, sizeof(context.trace_id.bytes)) &&
        bytes_valid(context.span_id.bytes, sizeof(context.span_id.bytes));
}

int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool parse_hex(const char* data, size_t data_len, uint8_t* out, size_t out_len)
{
    if (data == nullptr || out == nullptr || data_len != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        const int high = hex_value(data[i * 2]);
        const int low = hex_value(data[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

void format_hex(const uint8_t* data, size_t data_len, char* out)
{
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < data_len; ++i) {
        out[i * 2] = digits[(data[i] >> 4U) & 0x0fU];
        out[i * 2 + 1] = digits[data[i] & 0x0fU];
    }
}

galay_status_t random_bytes(uint8_t* out, size_t size)
{
    if (out == nullptr || size == 0 || size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (RAND_bytes(out, static_cast<int>(size)) != 1) return GALAY_INTERNAL_ERROR;
    if (!bytes_valid(out, size)) out[size - 1] = 1;
    return GALAY_OK;
}

bool valid_span_status(galay_tracing_span_status_code_t code)
{
    return code == GALAY_TRACING_SPAN_STATUS_UNSET ||
        code == GALAY_TRACING_SPAN_STATUS_OK ||
        code == GALAY_TRACING_SPAN_STATUS_ERROR;
}

bool valid_log_level(galay_tracing_log_level_t level)
{
    return level >= GALAY_TRACING_LOG_TRACE && level <= GALAY_TRACING_LOG_OFF;
}

bool valid_sampler_kind(galay_tracing_sampler_kind_t kind)
{
    return kind == GALAY_TRACING_SAMPLER_ALWAYS_ON ||
        kind == GALAY_TRACING_SAMPLER_ALWAYS_OFF ||
        kind == GALAY_TRACING_SAMPLER_TRACE_ID_RATIO;
}

galay_status_t copy_string(std::string_view value, char* out, size_t out_len, size_t* written)
{
    if (written != nullptr) *written = value.size();
    if (out == nullptr || written == nullptr || out_len < value.size()) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (!value.empty()) std::memcpy(out, value.data(), value.size());
    return GALAY_OK;
}

galay_status_t convert_attribute(const galay_tracing_attribute_t* input, TraceAttribute* out)
{
    if (input == nullptr || out == nullptr || input->name == nullptr || input->name_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (input->type < GALAY_TRACING_ATTRIBUTE_STRING || input->type > GALAY_TRACING_ATTRIBUTE_BOOL) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (input->type == GALAY_TRACING_ATTRIBUTE_STRING &&
        ((input->value == nullptr && input->value_len != 0) ||
         input->value_len > GALAY_TRACING_MAX_ATTRIBUTE_VALUE_LENGTH)) {
        return GALAY_INVALID_ARGUMENT;
    }
    out->name.assign(input->name, input->name_len);
    out->type = input->type;
    out->int64_value = input->int64_value;
    out->uint64_value = input->uint64_value;
    out->double_value = input->double_value;
    out->bool_value = input->bool_value == GALAY_TRUE;
    if (input->type == GALAY_TRACING_ATTRIBUTE_STRING) {
        out->string_value.assign(input->value == nullptr ? "" : input->value, input->value_len);
    }
    return GALAY_OK;
}

galay_status_t convert_attributes(const galay_tracing_attribute_t* attributes,
                                  size_t attribute_count,
                                  std::vector<TraceAttribute>* out)
{
    if (out == nullptr || (attributes == nullptr && attribute_count != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    out->clear();
    out->reserve(attribute_count);
    for (size_t i = 0; i < attribute_count; ++i) {
        TraceAttribute attribute;
        const galay_status_t status = convert_attribute(attributes + i, &attribute);
        if (status != GALAY_OK) return status;
        out->push_back(std::move(attribute));
    }
    return GALAY_OK;
}

void append_json_string(std::string& out, std::string_view value)
{
    static constexpr char digits[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (ch < 0x20) {
                    out.append("\\u00");
                    out.push_back(digits[ch >> 4U]);
                    out.push_back(digits[ch & 0x0fU]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    out.push_back('"');
}

void append_attribute_value(std::string& out, const TraceAttribute& attribute)
{
    switch (attribute.type) {
        case GALAY_TRACING_ATTRIBUTE_STRING:
            append_json_string(out, attribute.string_value);
            break;
        case GALAY_TRACING_ATTRIBUTE_INT64:
            out.append(std::to_string(attribute.int64_value));
            break;
        case GALAY_TRACING_ATTRIBUTE_UINT64:
            out.append(std::to_string(attribute.uint64_value));
            break;
        case GALAY_TRACING_ATTRIBUTE_DOUBLE:
            out.append(std::to_string(attribute.double_value));
            break;
        case GALAY_TRACING_ATTRIBUTE_BOOL:
            out.append(attribute.bool_value ? "true" : "false");
            break;
    }
}

void append_attributes(std::string& out, const std::vector<TraceAttribute>& attributes)
{
    out.push_back('[');
    for (size_t i = 0; i < attributes.size(); ++i) {
        if (i != 0) out.push_back(',');
        out.append("{\"key\":");
        append_json_string(out, attributes[i].name);
        out.append(",\"value\":");
        append_attribute_value(out, attributes[i]);
        out.push_back('}');
    }
    out.push_back(']');
}

std::string trace_id_hex(const galay_tracing_trace_id_t& id)
{
    std::string out(GALAY_TRACING_TRACE_ID_HEX_LENGTH, '0');
    format_hex(id.bytes, sizeof(id.bytes), out.data());
    return out;
}

std::string span_id_hex(const galay_tracing_span_id_t& id)
{
    std::string out(GALAY_TRACING_SPAN_ID_HEX_LENGTH, '0');
    format_hex(id.bytes, sizeof(id.bytes), out.data());
    return out;
}

std::string render_span_json(const galay_tracing_span_t& span)
{
    std::string line;
    line.append("{\"name\":");
    append_json_string(line, span.name);
    line.append(",\"trace_id\":\"");
    line.append(trace_id_hex(span.context.trace_id));
    line.append("\",\"span_id\":\"");
    line.append(span_id_hex(span.context.span_id));
    line.append("\",\"sampled\":");
    line.append((span.context.flags & 1U) != 0 ? "true" : "false");
    line.append(",\"status\":{\"code\":");
    append_json_string(line,
                       span.status == GALAY_TRACING_SPAN_STATUS_ERROR
                           ? "error"
                           : (span.status == GALAY_TRACING_SPAN_STATUS_OK ? "ok" : "unset"));
    line.append(",\"message\":");
    append_json_string(line, span.status_message);
    line.append("},\"attributes\":");
    append_attributes(line, span.attributes);
    line.append(",\"events\":[");
    for (size_t i = 0; i < span.events.size(); ++i) {
        if (i != 0) line.push_back(',');
        line.append("{\"name\":");
        append_json_string(line, span.events[i].name);
        line.append(",\"attributes\":");
        append_attributes(line, span.events[i].attributes);
        line.push_back('}');
    }
    line.append("],\"links\":[");
    for (size_t i = 0; i < span.links.size(); ++i) {
        if (i != 0) line.push_back(',');
        line.append("{\"trace_id\":\"");
        line.append(trace_id_hex(span.links[i].context.trace_id));
        line.append("\",\"span_id\":\"");
        line.append(span_id_hex(span.links[i].context.span_id));
        line.append("\",\"attributes\":");
        append_attributes(line, span.links[i].attributes);
        line.push_back('}');
    }
    line.append("]}");
    return line;
}

extern "C" {

const char* galay_tracing_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_tracing_trace_id_generate(galay_tracing_trace_id_t* out)
{
    return out == nullptr ? GALAY_INVALID_ARGUMENT : random_bytes(out->bytes, sizeof(out->bytes));
}

galay_bool_t galay_tracing_trace_id_is_valid(const galay_tracing_trace_id_t* id)
{
    return id != nullptr && bytes_valid(id->bytes, sizeof(id->bytes)) ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_tracing_trace_id_format(const galay_tracing_trace_id_t* id,
                                              char* out,
                                              size_t out_len,
                                              size_t* written)
{
    if (written != nullptr) *written = GALAY_TRACING_TRACE_ID_HEX_LENGTH;
    if (id == nullptr || out == nullptr || written == nullptr ||
        out_len < GALAY_TRACING_TRACE_ID_HEX_LENGTH) return GALAY_INVALID_ARGUMENT;
    format_hex(id->bytes, sizeof(id->bytes), out);
    return GALAY_OK;
}

galay_status_t galay_tracing_trace_id_parse(const char* data,
                                             size_t data_len,
                                             galay_tracing_trace_id_t* out)
{
    if (out == nullptr || !parse_hex(data, data_len, out->bytes, sizeof(out->bytes)) ||
        !bytes_valid(out->bytes, sizeof(out->bytes))) return GALAY_INVALID_ARGUMENT;
    return GALAY_OK;
}

galay_status_t galay_tracing_span_id_generate(galay_tracing_span_id_t* out)
{
    return out == nullptr ? GALAY_INVALID_ARGUMENT : random_bytes(out->bytes, sizeof(out->bytes));
}

galay_bool_t galay_tracing_span_id_is_valid(const galay_tracing_span_id_t* id)
{
    return id != nullptr && bytes_valid(id->bytes, sizeof(id->bytes)) ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_tracing_span_id_format(const galay_tracing_span_id_t* id,
                                             char* out,
                                             size_t out_len,
                                             size_t* written)
{
    if (written != nullptr) *written = GALAY_TRACING_SPAN_ID_HEX_LENGTH;
    if (id == nullptr || out == nullptr || written == nullptr ||
        out_len < GALAY_TRACING_SPAN_ID_HEX_LENGTH) return GALAY_INVALID_ARGUMENT;
    format_hex(id->bytes, sizeof(id->bytes), out);
    return GALAY_OK;
}

galay_status_t galay_tracing_span_id_parse(const char* data,
                                            size_t data_len,
                                            galay_tracing_span_id_t* out)
{
    if (out == nullptr || !parse_hex(data, data_len, out->bytes, sizeof(out->bytes)) ||
        !bytes_valid(out->bytes, sizeof(out->bytes))) return GALAY_INVALID_ARGUMENT;
    return GALAY_OK;
}

galay_status_t galay_tracing_traceparent_parse(const char* data,
                                                size_t data_len,
                                                const char* tracestate,
                                                size_t tracestate_len,
                                                galay_tracing_trace_context_t** out)
{
    return galay_tracing_trace_context_extract(data, data_len, tracestate, tracestate_len, out);
}

galay_status_t galay_tracing_traceparent_format(const galay_tracing_trace_context_t* context,
                                                 char* out,
                                                 size_t out_len,
                                                 size_t* written)
{
    if (written != nullptr) *written = GALAY_TRACING_TRACEPARENT_LENGTH;
    if (context == nullptr || !context_valid(context->context) || out == nullptr ||
        written == nullptr || out_len < GALAY_TRACING_TRACEPARENT_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    out[0] = '0';
    out[1] = '0';
    out[2] = '-';
    format_hex(context->context.trace_id.bytes, sizeof(context->context.trace_id.bytes), out + 3);
    out[35] = '-';
    format_hex(context->context.span_id.bytes, sizeof(context->context.span_id.bytes), out + 36);
    out[52] = '-';
    static constexpr char digits[] = "0123456789abcdef";
    out[53] = digits[(context->context.flags >> 4U) & 0x0fU];
    out[54] = digits[context->context.flags & 0x0fU];
    return GALAY_OK;
}

galay_status_t galay_tracing_trace_context_extract(const char* traceparent,
                                                    size_t traceparent_len,
                                                    const char* tracestate,
                                                    size_t tracestate_len,
                                                    galay_tracing_trace_context_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (traceparent == nullptr || out == nullptr ||
        (tracestate == nullptr && tracestate_len != 0) ||
        traceparent_len != GALAY_TRACING_TRACEPARENT_LENGTH ||
        traceparent[0] != '0' || traceparent[1] != '0' ||
        traceparent[2] != '-' || traceparent[35] != '-' || traceparent[52] != '-') {
        return GALAY_PROTOCOL_ERROR;
    }
    TraceContextData parsed;
    if (!parse_hex(traceparent + 3, 32, parsed.trace_id.bytes, sizeof(parsed.trace_id.bytes)) ||
        !parse_hex(traceparent + 36, 16, parsed.span_id.bytes, sizeof(parsed.span_id.bytes)) ||
        !parse_hex(traceparent + 53, 2, &parsed.flags, 1) || !context_valid(parsed)) {
        return GALAY_PROTOCOL_ERROR;
    }
    if (tracestate_len > 512) return GALAY_PROTOCOL_ERROR;
    for (size_t i = 0; i < tracestate_len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(tracestate[i]);
        if (ch < 0x20 || ch > 0x7e) return GALAY_PROTOCOL_ERROR;
    }
    parsed.tracestate.assign(tracestate == nullptr ? "" : tracestate, tracestate_len);
    auto* context = new (std::nothrow) galay_tracing_trace_context_t();
    if (context == nullptr) return GALAY_OUT_OF_MEMORY;
    context->context = std::move(parsed);
    *out = context;
    return GALAY_OK;
}

galay_status_t galay_tracing_trace_context_inject(const galay_tracing_trace_context_t* context,
                                                   char* traceparent_out,
                                                   size_t traceparent_out_len,
                                                   size_t* traceparent_written,
                                                   char* tracestate_out,
                                                   size_t tracestate_out_len,
                                                   size_t* tracestate_written)
{
    const galay_status_t traceparent_status = galay_tracing_traceparent_format(
        context, traceparent_out, traceparent_out_len, traceparent_written);
    if (traceparent_status != GALAY_OK) return traceparent_status;
    return copy_string(context->context.tracestate,
                       tracestate_out,
                       tracestate_out_len,
                       tracestate_written);
}

void galay_tracing_trace_context_destroy(galay_tracing_trace_context_t** context)
{
    if (context == nullptr || *context == nullptr) return;
    delete *context;
    *context = nullptr;
}

galay_status_t galay_tracing_trace_context_flags(const galay_tracing_trace_context_t* context,
                                                  uint8_t* flags)
{
    if (context == nullptr || flags == nullptr || !context_valid(context->context)) {
        return GALAY_INVALID_ARGUMENT;
    }
    *flags = context->context.flags;
    return GALAY_OK;
}

galay_status_t galay_tracing_provider_create(galay_tracing_provider_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_tracing_provider_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_tracing_provider_destroy(galay_tracing_provider_t** provider)
{
    if (provider == nullptr || *provider == nullptr) return;
    delete *provider;
    *provider = nullptr;
}

galay_status_t galay_tracing_provider_set_file_exporter(galay_tracing_provider_t* provider,
                                                         const char* path,
                                                         size_t path_len)
{
    if (provider == nullptr || path == nullptr || path_len == 0) return GALAY_INVALID_ARGUMENT;
    const std::lock_guard<std::mutex> lock(provider->mutex);
    if (provider->output.is_open()) provider->output.close();
    provider->output.clear();
    provider->output.open(std::string(path, path_len), std::ios::out | std::ios::app);
    provider->configured = true;
    provider->shutdown = false;
    return GALAY_OK;
}

galay_status_t galay_tracing_provider_force_flush(galay_tracing_provider_t* provider,
                                                   int64_t timeout_ms)
{
    if (provider == nullptr || !provider->configured || timeout_ms < -1) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::lock_guard<std::mutex> lock(provider->mutex);
    if (!provider->output) return GALAY_IO_ERROR;
    provider->output.flush();
    return provider->output ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t galay_tracing_provider_shutdown(galay_tracing_provider_t* provider,
                                                int64_t timeout_ms)
{
    if (provider == nullptr || !provider->configured || timeout_ms < -1) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::lock_guard<std::mutex> lock(provider->mutex);
    if (provider->shutdown) return GALAY_OK;
    bool ok = static_cast<bool>(provider->output);
    if (provider->output.is_open()) {
        provider->output.flush();
        ok = ok && static_cast<bool>(provider->output);
        provider->output.close();
    }
    provider->shutdown = true;
    return ok ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t galay_tracing_tracer_create(galay_tracing_provider_t* provider,
                                            const char* name,
                                            size_t name_len,
                                            galay_tracing_tracer_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (provider == nullptr || name == nullptr || name_len == 0 || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* tracer = new (std::nothrow) galay_tracing_tracer_t();
    if (tracer == nullptr) return GALAY_OUT_OF_MEMORY;
    tracer->provider = provider;
    tracer->name.assign(name, name_len);
    *out = tracer;
    return GALAY_OK;
}

void galay_tracing_tracer_destroy(galay_tracing_tracer_t** tracer)
{
    if (tracer == nullptr || *tracer == nullptr) return;
    delete *tracer;
    *tracer = nullptr;
}

galay_status_t galay_tracing_tracer_start_span(galay_tracing_tracer_t* tracer,
                                                const char* name,
                                                size_t name_len,
                                                const galay_tracing_trace_context_t* context,
                                                galay_tracing_span_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (tracer == nullptr || tracer->provider == nullptr || name == nullptr || name_len == 0 ||
        context == nullptr || !context_valid(context->context) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* span = new (std::nothrow) galay_tracing_span_t();
    if (span == nullptr) return GALAY_OUT_OF_MEMORY;
    span->provider = tracer->provider;
    span->name.assign(name, name_len);
    span->context = context->context;
    span->context.parent_span_id = context->context.span_id;
    const galay_status_t generated = galay_tracing_span_id_generate(&span->context.span_id);
    if (generated != GALAY_OK) {
        delete span;
        return generated;
    }
    *out = span;
    return GALAY_OK;
}

void galay_tracing_span_destroy(galay_tracing_span_t** span)
{
    if (span == nullptr || *span == nullptr) return;
    delete *span;
    *span = nullptr;
}

galay_status_t galay_tracing_span_add_event(galay_tracing_span_t* span,
                                             const char* name,
                                             size_t name_len,
                                             const galay_tracing_attribute_t* attributes,
                                             size_t attribute_count)
{
    if (span == nullptr || span->ended || name == nullptr || name_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    TraceEvent event;
    event.name.assign(name, name_len);
    const galay_status_t converted = convert_attributes(attributes, attribute_count, &event.attributes);
    if (converted != GALAY_OK) return converted;
    span->events.push_back(std::move(event));
    return GALAY_OK;
}

galay_status_t galay_tracing_span_set_attribute(galay_tracing_span_t* span,
                                                 const galay_tracing_attribute_t* attribute)
{
    if (span == nullptr || span->ended || attribute == nullptr) return GALAY_INVALID_ARGUMENT;
    TraceAttribute converted;
    const galay_status_t status = convert_attribute(attribute, &converted);
    if (status != GALAY_OK) return status;
    for (TraceAttribute& current : span->attributes) {
        if (current.name == converted.name) {
            current = std::move(converted);
            return GALAY_OK;
        }
    }
    span->attributes.push_back(std::move(converted));
    return GALAY_OK;
}

galay_status_t galay_tracing_span_set_status(galay_tracing_span_t* span,
                                              galay_tracing_span_status_code_t code,
                                              const char* message,
                                              size_t message_len)
{
    if (span == nullptr || span->ended || !valid_span_status(code) ||
        (message == nullptr && message_len != 0)) return GALAY_INVALID_ARGUMENT;
    span->status = code;
    span->status_message.assign(message == nullptr ? "" : message, message_len);
    return GALAY_OK;
}

galay_status_t galay_tracing_span_add_link(galay_tracing_span_t* span,
                                            const galay_tracing_trace_context_t* context,
                                            const galay_tracing_attribute_t* attributes,
                                            size_t attribute_count)
{
    if (span == nullptr || span->ended || context == nullptr || !context_valid(context->context)) {
        return GALAY_INVALID_ARGUMENT;
    }
    TraceLink link;
    link.context = context->context;
    const galay_status_t converted = convert_attributes(attributes, attribute_count, &link.attributes);
    if (converted != GALAY_OK) return converted;
    span->links.push_back(std::move(link));
    return GALAY_OK;
}

galay_status_t galay_tracing_span_attribute_count(const galay_tracing_span_t* span, size_t* out)
{
    if (span == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = span->attributes.size();
    return GALAY_OK;
}

galay_status_t galay_tracing_span_event_count(const galay_tracing_span_t* span, size_t* out)
{
    if (span == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = span->events.size();
    return GALAY_OK;
}

galay_status_t galay_tracing_span_link_count(const galay_tracing_span_t* span, size_t* out)
{
    if (span == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = span->links.size();
    return GALAY_OK;
}

galay_status_t galay_tracing_span_status(const galay_tracing_span_t* span,
                                          galay_tracing_span_status_code_t* out)
{
    if (span == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = span->status;
    return GALAY_OK;
}

galay_status_t galay_tracing_span_end(galay_tracing_span_t* span)
{
    if (span == nullptr || span->ended) return GALAY_INVALID_ARGUMENT;
    span->ended = true;
    if (span->provider != nullptr) {
        const std::lock_guard<std::mutex> lock(span->provider->mutex);
        if (span->provider->configured && !span->provider->shutdown && span->provider->output) {
            span->provider->output << render_span_json(*span) << '\n';
        }
    }
    return GALAY_OK;
}

galay_status_t galay_tracing_sampler_create(galay_tracing_sampler_kind_t kind,
                                             double ratio,
                                             galay_tracing_sampler_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (out == nullptr || !valid_sampler_kind(kind) ||
        (kind == GALAY_TRACING_SAMPLER_TRACE_ID_RATIO && (ratio < 0.0 || ratio > 1.0))) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* sampler = new (std::nothrow) galay_tracing_sampler_t();
    if (sampler == nullptr) return GALAY_OUT_OF_MEMORY;
    sampler->kind = kind;
    sampler->ratio = ratio;
    *out = sampler;
    return GALAY_OK;
}

void galay_tracing_sampler_destroy(galay_tracing_sampler_t** sampler)
{
    if (sampler == nullptr || *sampler == nullptr) return;
    delete *sampler;
    *sampler = nullptr;
}

galay_status_t galay_tracing_sampler_should_sample(const galay_tracing_sampler_t* sampler,
                                                    const galay_tracing_trace_context_t* context,
                                                    galay_bool_t* out)
{
    if (sampler == nullptr || context == nullptr || !context_valid(context->context) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (sampler->kind == GALAY_TRACING_SAMPLER_ALWAYS_ON) {
        *out = GALAY_TRUE;
    } else if (sampler->kind == GALAY_TRACING_SAMPLER_ALWAYS_OFF || sampler->ratio <= 0.0) {
        *out = GALAY_FALSE;
    } else if (sampler->ratio >= 1.0) {
        *out = GALAY_TRUE;
    } else {
        uint64_t prefix = 0;
        for (size_t i = 0; i < 8; ++i) {
            prefix = (prefix << 8U) | context->context.trace_id.bytes[i];
        }
        const long double normalized = static_cast<long double>(prefix) /
            static_cast<long double>(std::numeric_limits<uint64_t>::max());
        *out = normalized < sampler->ratio ? GALAY_TRUE : GALAY_FALSE;
    }
    return GALAY_OK;
}

galay_status_t galay_tracing_logger_create_file(const char* path,
                                                 size_t path_len,
                                                 galay_tracing_log_level_t level,
                                                 galay_tracing_logger_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (path == nullptr || path_len == 0 || !valid_log_level(level) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* logger = new (std::nothrow) galay_tracing_logger_t();
    if (logger == nullptr) return GALAY_OUT_OF_MEMORY;
    logger->level = level;
    logger->output.open(std::string(path, path_len), std::ios::out | std::ios::app);
    *out = logger;
    return GALAY_OK;
}

galay_status_t galay_tracing_logger_log(galay_tracing_logger_t* logger,
                                         galay_tracing_log_level_t level,
                                         const galay_tracing_trace_context_t* context,
                                         const char* message,
                                         size_t message_len)
{
    if (logger == nullptr || !valid_log_level(level) || message == nullptr || message_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (logger->level == GALAY_TRACING_LOG_OFF || level < logger->level) return GALAY_OK;
    const std::lock_guard<std::mutex> lock(logger->mutex);
    logger->output.write(message, static_cast<std::streamsize>(message_len));
    if (context != nullptr && context_valid(context->context)) {
        logger->output << " trace_id=" << trace_id_hex(context->context.trace_id)
                       << " span_id=" << span_id_hex(context->context.span_id);
    }
    logger->output << '\n';
    logger->output.flush();
    return GALAY_OK;
}

void galay_tracing_logger_destroy(galay_tracing_logger_t** logger)
{
    if (logger == nullptr || *logger == nullptr) return;
    delete *logger;
    *logger = nullptr;
}

}
