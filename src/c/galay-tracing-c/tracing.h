#ifndef GALAY_C_TRACING_TRACING_H
#define GALAY_C_TRACING_TRACING_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GALAY_TRACING_TRACE_ID_HEX_LENGTH 32u
#define GALAY_TRACING_SPAN_ID_HEX_LENGTH 16u
#define GALAY_TRACING_TRACEPARENT_LENGTH 55u
#define GALAY_TRACING_MAX_ATTRIBUTE_VALUE_LENGTH 256u

typedef struct galay_tracing_trace_id_t { uint8_t bytes[16]; } galay_tracing_trace_id_t;
typedef struct galay_tracing_span_id_t { uint8_t bytes[8]; } galay_tracing_span_id_t;
typedef struct galay_tracing_trace_context_t galay_tracing_trace_context_t;
typedef struct galay_tracing_provider_t galay_tracing_provider_t;
typedef struct galay_tracing_tracer_t galay_tracing_tracer_t;
typedef struct galay_tracing_span_t galay_tracing_span_t;

typedef struct galay_tracing_attribute_t {
    const char* name;
    size_t name_len;
    const char* value;
    size_t value_len;
} galay_tracing_attribute_t;

const char* galay_tracing_get_error(galay_status_t status);
galay_status_t galay_tracing_trace_id_generate(galay_tracing_trace_id_t* out);
galay_bool_t galay_tracing_trace_id_is_valid(const galay_tracing_trace_id_t* id);
galay_status_t galay_tracing_trace_id_format(const galay_tracing_trace_id_t* id, char* out,
                                             size_t out_len, size_t* written);
galay_status_t galay_tracing_trace_id_parse(const char* data, size_t data_len,
                                            galay_tracing_trace_id_t* out);
galay_status_t galay_tracing_span_id_generate(galay_tracing_span_id_t* out);
galay_bool_t galay_tracing_span_id_is_valid(const galay_tracing_span_id_t* id);
galay_status_t galay_tracing_span_id_format(const galay_tracing_span_id_t* id, char* out,
                                            size_t out_len, size_t* written);
galay_status_t galay_tracing_span_id_parse(const char* data, size_t data_len,
                                           galay_tracing_span_id_t* out);
galay_status_t galay_tracing_traceparent_parse(const char* data, size_t data_len,
                                               const char* tracestate, size_t tracestate_len,
                                               galay_tracing_trace_context_t** out);
galay_status_t galay_tracing_traceparent_format(const galay_tracing_trace_context_t* context,
                                                char* out, size_t out_len, size_t* written);
void galay_tracing_trace_context_destroy(galay_tracing_trace_context_t** context);
galay_status_t galay_tracing_trace_context_flags(const galay_tracing_trace_context_t* context,
                                                 uint8_t* flags);
galay_status_t galay_tracing_provider_create(galay_tracing_provider_t** out);
void galay_tracing_provider_destroy(galay_tracing_provider_t** provider);
galay_status_t galay_tracing_tracer_create(galay_tracing_provider_t* provider, const char* name,
                                           size_t name_len, galay_tracing_tracer_t** out);
void galay_tracing_tracer_destroy(galay_tracing_tracer_t** tracer);
galay_status_t galay_tracing_tracer_start_span(galay_tracing_tracer_t* tracer, const char* name,
                                               size_t name_len,
                                               const galay_tracing_trace_context_t* context,
                                               galay_tracing_span_t** out);
void galay_tracing_span_destroy(galay_tracing_span_t** span);
galay_status_t galay_tracing_span_add_event(galay_tracing_span_t* span, const char* name,
                                            size_t name_len,
                                            const galay_tracing_attribute_t* attributes,
                                            size_t attribute_count);
galay_status_t galay_tracing_span_end(galay_tracing_span_t* span);

#ifdef __cplusplus
}
#endif

#endif
