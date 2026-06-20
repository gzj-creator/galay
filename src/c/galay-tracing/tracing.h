/**
 * @file tracing.h
 * @brief galay-tracing C ABI 封装。
 *
 * @details 该头文件可被 C11 和 C++ 编译器直接包含。TraceId/SpanId 以值类型
 *          暴露，TraceContext、Provider、Tracer 和 Span 通过 opaque handle
 *          管理；所有 create/parse/start 成功返回的 handle 均由调用方 destroy。
 */

#ifndef GALAY_C_TRACING_TRACING_H
#define GALAY_C_TRACING_TRACING_H

#include <galay/c/galay-c/common/galay_c_error.h>

GALAY_C_BEGIN_DECLS

enum {
    GALAY_TRACING_TRACE_ID_BYTE_LENGTH = 16,
    GALAY_TRACING_TRACE_ID_HEX_LENGTH = 32,
    GALAY_TRACING_SPAN_ID_BYTE_LENGTH = 8,
    GALAY_TRACING_SPAN_ID_HEX_LENGTH = 16,
    GALAY_TRACING_TRACEPARENT_LENGTH = 55,
    GALAY_TRACING_MAX_ATTRIBUTE_VALUE_LENGTH = 4096
};

typedef struct galay_tracing_trace_id {
    uint8_t bytes[GALAY_TRACING_TRACE_ID_BYTE_LENGTH];
} galay_tracing_trace_id_t;

typedef struct galay_tracing_span_id {
    uint8_t bytes[GALAY_TRACING_SPAN_ID_BYTE_LENGTH];
} galay_tracing_span_id_t;

typedef struct galay_tracing_attribute {
    const char* name;
    size_t name_len;
    const char* value;
    size_t value_len;
} galay_tracing_attribute_t;

typedef struct galay_tracing_trace_context galay_tracing_trace_context_t;
typedef struct galay_tracing_provider galay_tracing_provider_t;
typedef struct galay_tracing_tracer galay_tracing_tracer_t;
typedef struct galay_tracing_span galay_tracing_span_t;

GALAY_C_API galay_status_t galay_tracing_trace_id_generate(galay_tracing_trace_id_t* out);
GALAY_C_API galay_status_t galay_tracing_trace_id_parse(const char* hex,
                                                        size_t hex_len,
                                                        galay_tracing_trace_id_t* out);
GALAY_C_API galay_status_t galay_tracing_trace_id_format(const galay_tracing_trace_id_t* trace_id,
                                                         char* out,
                                                         size_t out_len,
                                                         size_t* written);
GALAY_C_API galay_bool_t galay_tracing_trace_id_is_valid(const galay_tracing_trace_id_t* trace_id);

GALAY_C_API galay_status_t galay_tracing_span_id_generate(galay_tracing_span_id_t* out);
GALAY_C_API galay_status_t galay_tracing_span_id_parse(const char* hex,
                                                       size_t hex_len,
                                                       galay_tracing_span_id_t* out);
GALAY_C_API galay_status_t galay_tracing_span_id_format(const galay_tracing_span_id_t* span_id,
                                                        char* out,
                                                        size_t out_len,
                                                        size_t* written);
GALAY_C_API galay_bool_t galay_tracing_span_id_is_valid(const galay_tracing_span_id_t* span_id);

GALAY_C_API galay_status_t galay_tracing_trace_context_create(const galay_tracing_trace_id_t* trace_id,
                                                              const galay_tracing_span_id_t* span_id,
                                                              uint8_t trace_flags,
                                                              const char* tracestate,
                                                              size_t tracestate_len,
                                                              galay_tracing_trace_context_t** out);
GALAY_C_API void galay_tracing_trace_context_destroy(galay_tracing_trace_context_t** context);
GALAY_C_API galay_status_t galay_tracing_trace_context_trace_id(const galay_tracing_trace_context_t* context,
                                                                galay_tracing_trace_id_t* out);
GALAY_C_API galay_status_t galay_tracing_trace_context_span_id(const galay_tracing_trace_context_t* context,
                                                               galay_tracing_span_id_t* out);
GALAY_C_API galay_status_t galay_tracing_trace_context_flags(const galay_tracing_trace_context_t* context,
                                                             uint8_t* out);

GALAY_C_API galay_status_t galay_tracing_traceparent_parse(const char* traceparent,
                                                           size_t traceparent_len,
                                                           const char* tracestate,
                                                           size_t tracestate_len,
                                                           galay_tracing_trace_context_t** out);
GALAY_C_API galay_status_t galay_tracing_traceparent_format(const galay_tracing_trace_context_t* context,
                                                            char* out,
                                                            size_t out_len,
                                                            size_t* written);

GALAY_C_API galay_status_t galay_tracing_provider_create(galay_tracing_provider_t** out);
GALAY_C_API void galay_tracing_provider_destroy(galay_tracing_provider_t** provider);
GALAY_C_API galay_status_t galay_tracing_tracer_create(galay_tracing_provider_t* provider,
                                                       const char* name,
                                                       size_t name_len,
                                                       galay_tracing_tracer_t** out);
GALAY_C_API void galay_tracing_tracer_destroy(galay_tracing_tracer_t** tracer);
GALAY_C_API galay_status_t galay_tracing_tracer_start_span(galay_tracing_tracer_t* tracer,
                                                           const char* name,
                                                           size_t name_len,
                                                           const galay_tracing_trace_context_t* context,
                                                           galay_tracing_span_t** out);
GALAY_C_API galay_status_t galay_tracing_span_add_event(galay_tracing_span_t* span,
                                                        const char* name,
                                                        size_t name_len,
                                                        const galay_tracing_attribute_t* attributes,
                                                        size_t attribute_count);
GALAY_C_API galay_status_t galay_tracing_span_end(galay_tracing_span_t* span);
GALAY_C_API void galay_tracing_span_destroy(galay_tracing_span_t** span);

GALAY_C_END_DECLS

#endif /* GALAY_C_TRACING_TRACING_H */
