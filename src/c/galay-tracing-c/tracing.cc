#include <galay/c/galay-tracing-c/tracing.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

struct galay_tracing_trace_context_t {
    galay_tracing_trace_id_t trace_id{};
    galay_tracing_span_id_t span_id{};
    uint8_t flags = 0;
};
struct galay_tracing_provider_t {};
struct galay_tracing_tracer_t { std::string name; };
struct galay_tracing_span_t { bool ended = false; };

static uint8_t g_next_id = 1;

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool bytes_all_zero(const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (data[i] != 0) return false;
    }
    return true;
}

static galay_status_t parse_hex(const char* data, size_t data_len, uint8_t* out, size_t out_len)
{
    if (data == nullptr || out == nullptr || data_len != out_len * 2) return GALAY_INVALID_ARGUMENT;
    for (size_t i = 0; i < out_len; ++i) {
        const int hi = hex_value(data[i * 2]);
        const int lo = hex_value(data[i * 2 + 1]);
        if (hi < 0 || lo < 0) return GALAY_INVALID_ARGUMENT;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return bytes_all_zero(out, out_len) ? GALAY_INVALID_ARGUMENT : GALAY_OK;
}

static galay_status_t format_hex(const uint8_t* data, size_t data_len, char* out,
                                 size_t out_len, size_t* written)
{
    static constexpr char hex[] = "0123456789abcdef";
    const size_t need = data_len * 2;
    if (written != nullptr) *written = need;
    if (data == nullptr || out == nullptr || written == nullptr) return GALAY_INVALID_ARGUMENT;
    if (out_len < need) return GALAY_INVALID_ARGUMENT;
    for (size_t i = 0; i < data_len; ++i) {
        out[i * 2] = hex[(data[i] >> 4U) & 0x0FU];
        out[i * 2 + 1] = hex[data[i] & 0x0FU];
    }
    return GALAY_OK;
}

extern "C" {

const char* galay_tracing_get_error(galay_status_t status) { return galay_status_string(status); }

galay_status_t galay_tracing_trace_id_generate(galay_tracing_trace_id_t* out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    for (uint8_t& byte : out->bytes) byte = g_next_id++;
    return GALAY_OK;
}

galay_bool_t galay_tracing_trace_id_is_valid(const galay_tracing_trace_id_t* id)
{
    return id != nullptr && !bytes_all_zero(id->bytes, sizeof(id->bytes)) ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_tracing_trace_id_format(const galay_tracing_trace_id_t* id, char* out,
                                             size_t out_len, size_t* written)
{
    return id == nullptr ? GALAY_INVALID_ARGUMENT : format_hex(id->bytes, sizeof(id->bytes), out, out_len, written);
}

galay_status_t galay_tracing_trace_id_parse(const char* data, size_t data_len,
                                            galay_tracing_trace_id_t* out)
{
    return out == nullptr ? GALAY_INVALID_ARGUMENT : parse_hex(data, data_len, out->bytes, sizeof(out->bytes));
}

galay_status_t galay_tracing_span_id_generate(galay_tracing_span_id_t* out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    for (uint8_t& byte : out->bytes) byte = g_next_id++;
    return GALAY_OK;
}

galay_bool_t galay_tracing_span_id_is_valid(const galay_tracing_span_id_t* id)
{
    return id != nullptr && !bytes_all_zero(id->bytes, sizeof(id->bytes)) ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_tracing_span_id_format(const galay_tracing_span_id_t* id, char* out,
                                            size_t out_len, size_t* written)
{
    return id == nullptr ? GALAY_INVALID_ARGUMENT : format_hex(id->bytes, sizeof(id->bytes), out, out_len, written);
}

galay_status_t galay_tracing_span_id_parse(const char* data, size_t data_len,
                                           galay_tracing_span_id_t* out)
{
    return out == nullptr ? GALAY_INVALID_ARGUMENT : parse_hex(data, data_len, out->bytes, sizeof(out->bytes));
}

galay_status_t galay_tracing_traceparent_parse(const char* data, size_t data_len,
                                               const char*, size_t,
                                               galay_tracing_trace_context_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (data == nullptr || out == nullptr || data_len != GALAY_TRACING_TRACEPARENT_LENGTH ||
        data[2] != '-' || data[35] != '-' || data[52] != '-') {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* context = new (std::nothrow) galay_tracing_trace_context_t();
    if (context == nullptr) return GALAY_OUT_OF_MEMORY;
    galay_status_t status = galay_tracing_trace_id_parse(data + 3, 32, &context->trace_id);
    if (status != GALAY_OK) {
        delete context;
        return GALAY_PROTOCOL_ERROR;
    }
    status = galay_tracing_span_id_parse(data + 36, 16, &context->span_id);
    if (status != GALAY_OK) {
        delete context;
        return GALAY_PROTOCOL_ERROR;
    }
    const int hi = hex_value(data[53]);
    const int lo = hex_value(data[54]);
    if (hi < 0 || lo < 0) {
        delete context;
        return GALAY_PROTOCOL_ERROR;
    }
    context->flags = static_cast<uint8_t>((hi << 4) | lo);
    *out = context;
    return GALAY_OK;
}

galay_status_t galay_tracing_traceparent_format(const galay_tracing_trace_context_t* context,
                                                char* out, size_t out_len, size_t* written)
{
    if (written != nullptr) *written = GALAY_TRACING_TRACEPARENT_LENGTH;
    if (context == nullptr || out == nullptr || written == nullptr) return GALAY_INVALID_ARGUMENT;
    if (out_len < GALAY_TRACING_TRACEPARENT_LENGTH) return GALAY_INVALID_ARGUMENT;
    out[0] = '0'; out[1] = '0'; out[2] = '-';
    size_t ignored = 0;
    galay_status_t status = galay_tracing_trace_id_format(&context->trace_id, out + 3, 32, &ignored);
    if (status != GALAY_OK) return status;
    out[35] = '-';
    status = galay_tracing_span_id_format(&context->span_id, out + 36, 16, &ignored);
    if (status != GALAY_OK) return status;
    out[52] = '-';
    const int printed = std::snprintf(out + 53, 3, "%02x", context->flags);
    return printed == 2 ? GALAY_OK : GALAY_INTERNAL_ERROR;
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
    if (context == nullptr || flags == nullptr) return GALAY_INVALID_ARGUMENT;
    *flags = context->flags;
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

galay_status_t galay_tracing_tracer_create(galay_tracing_provider_t* provider, const char* name,
                                           size_t name_len, galay_tracing_tracer_t** out)
{
    if (provider == nullptr || name == nullptr || name_len == 0 || out == nullptr) return GALAY_INVALID_ARGUMENT;
    auto* tracer = new (std::nothrow) galay_tracing_tracer_t();
    if (tracer == nullptr) return GALAY_OUT_OF_MEMORY;
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

galay_status_t galay_tracing_tracer_start_span(galay_tracing_tracer_t* tracer, const char* name,
                                               size_t name_len,
                                               const galay_tracing_trace_context_t* context,
                                               galay_tracing_span_t** out)
{
    if (tracer == nullptr || name == nullptr || name_len == 0 || context == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_tracing_span_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_tracing_span_destroy(galay_tracing_span_t** span)
{
    if (span == nullptr || *span == nullptr) return;
    delete *span;
    *span = nullptr;
}

galay_status_t galay_tracing_span_add_event(galay_tracing_span_t* span, const char* name,
                                            size_t name_len,
                                            const galay_tracing_attribute_t* attributes,
                                            size_t attribute_count)
{
    if (span == nullptr || name == nullptr || name_len == 0) return GALAY_INVALID_ARGUMENT;
    for (size_t i = 0; i < attribute_count; ++i) {
        if (attributes == nullptr || attributes[i].value_len > GALAY_TRACING_MAX_ATTRIBUTE_VALUE_LENGTH) {
            return GALAY_INVALID_ARGUMENT;
        }
    }
    return GALAY_OK;
}

galay_status_t galay_tracing_span_end(galay_tracing_span_t* span)
{
    if (span == nullptr || span->ended) return GALAY_INVALID_ARGUMENT;
    span->ended = true;
    return GALAY_OK;
}

}
