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
typedef struct galay_tracing_sampler_t galay_tracing_sampler_t;
typedef struct galay_tracing_logger_t galay_tracing_logger_t;

typedef enum galay_tracing_attribute_type_t {
    GALAY_TRACING_ATTRIBUTE_STRING = 0,
    GALAY_TRACING_ATTRIBUTE_INT64 = 1,
    GALAY_TRACING_ATTRIBUTE_UINT64 = 2,
    GALAY_TRACING_ATTRIBUTE_DOUBLE = 3,
    GALAY_TRACING_ATTRIBUTE_BOOL = 4
} galay_tracing_attribute_type_t;

typedef enum galay_tracing_span_status_code_t {
    GALAY_TRACING_SPAN_STATUS_UNSET = 0,
    GALAY_TRACING_SPAN_STATUS_OK = 1,
    GALAY_TRACING_SPAN_STATUS_ERROR = 2
} galay_tracing_span_status_code_t;

typedef enum galay_tracing_sampler_kind_t {
    GALAY_TRACING_SAMPLER_ALWAYS_ON = 0,
    GALAY_TRACING_SAMPLER_ALWAYS_OFF = 1,
    GALAY_TRACING_SAMPLER_TRACE_ID_RATIO = 2
} galay_tracing_sampler_kind_t;

typedef enum galay_tracing_log_level_t {
    GALAY_TRACING_LOG_TRACE = 0,
    GALAY_TRACING_LOG_DEBUG = 1,
    GALAY_TRACING_LOG_INFO = 2,
    GALAY_TRACING_LOG_WARN = 3,
    GALAY_TRACING_LOG_ERROR = 4,
    GALAY_TRACING_LOG_OFF = 5
} galay_tracing_log_level_t;

typedef struct galay_tracing_attribute_t {
    const char* name;
    size_t name_len;
    const char* value;
    size_t value_len;
    galay_tracing_attribute_type_t type;
    int64_t int64_value;
    uint64_t uint64_value;
    double double_value;
    galay_bool_t bool_value;
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
galay_status_t galay_tracing_trace_context_extract(const char* traceparent, size_t traceparent_len,
                                                   const char* tracestate, size_t tracestate_len,
                                                   galay_tracing_trace_context_t** out);
galay_status_t galay_tracing_trace_context_inject(const galay_tracing_trace_context_t* context,
                                                  char* traceparent_out, size_t traceparent_out_len,
                                                  size_t* traceparent_written,
                                                  char* tracestate_out, size_t tracestate_out_len,
                                                  size_t* tracestate_written);
void galay_tracing_trace_context_destroy(galay_tracing_trace_context_t** context);
galay_status_t galay_tracing_trace_context_flags(const galay_tracing_trace_context_t* context,
                                                 uint8_t* flags);
galay_status_t galay_tracing_provider_create(galay_tracing_provider_t** out);
void galay_tracing_provider_destroy(galay_tracing_provider_t** provider);
galay_status_t galay_tracing_provider_set_file_exporter(galay_tracing_provider_t* provider,
                                                        const char* path, size_t path_len);
galay_status_t galay_tracing_provider_force_flush(galay_tracing_provider_t* provider,
                                                  int64_t timeout_ms);
galay_status_t galay_tracing_provider_shutdown(galay_tracing_provider_t* provider,
                                               int64_t timeout_ms);
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
galay_status_t galay_tracing_span_set_attribute(galay_tracing_span_t* span,
                                                const galay_tracing_attribute_t* attribute);
galay_status_t galay_tracing_span_set_status(galay_tracing_span_t* span,
                                             galay_tracing_span_status_code_t code,
                                             const char* message, size_t message_len);
galay_status_t galay_tracing_span_add_link(galay_tracing_span_t* span,
                                           const galay_tracing_trace_context_t* context,
                                           const galay_tracing_attribute_t* attributes,
                                           size_t attribute_count);
galay_status_t galay_tracing_span_attribute_count(const galay_tracing_span_t* span, size_t* out);
galay_status_t galay_tracing_span_event_count(const galay_tracing_span_t* span, size_t* out);
galay_status_t galay_tracing_span_link_count(const galay_tracing_span_t* span, size_t* out);
galay_status_t galay_tracing_span_status(const galay_tracing_span_t* span,
                                         galay_tracing_span_status_code_t* out);
galay_status_t galay_tracing_span_end(galay_tracing_span_t* span);
galay_status_t galay_tracing_sampler_create(galay_tracing_sampler_kind_t kind, double ratio,
                                            galay_tracing_sampler_t** out);
void galay_tracing_sampler_destroy(galay_tracing_sampler_t** sampler);
galay_status_t galay_tracing_sampler_should_sample(const galay_tracing_sampler_t* sampler,
                                                   const galay_tracing_trace_context_t* context,
                                                   galay_bool_t* out);
galay_status_t galay_tracing_logger_create_file(const char* path, size_t path_len,
                                                galay_tracing_log_level_t level,
                                                galay_tracing_logger_t** out);
galay_status_t galay_tracing_logger_log(galay_tracing_logger_t* logger,
                                        galay_tracing_log_level_t level,
                                        const galay_tracing_trace_context_t* context,
                                        const char* message, size_t message_len);
void galay_tracing_logger_destroy(galay_tracing_logger_t** logger);

#ifdef __cplusplus
}
#endif

#endif
