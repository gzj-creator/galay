/**
 * @file macro.h
 * @brief galay-tracing C ABI 编译期长度宏
 */

#ifndef GALAY_C_TRACING_MACRO_H
#define GALAY_C_TRACING_MACRO_H

/** @brief TraceId 十六进制文本长度，不包含 NUL。 */
#ifndef GALAY_TRACING_TRACE_ID_HEX_LENGTH
#define GALAY_TRACING_TRACE_ID_HEX_LENGTH 32u
#endif
/** @brief SpanId 十六进制文本长度，不包含 NUL。 */
#ifndef GALAY_TRACING_SPAN_ID_HEX_LENGTH
#define GALAY_TRACING_SPAN_ID_HEX_LENGTH 16u
#endif
/** @brief W3C traceparent 文本长度，不包含 NUL。 */
#ifndef GALAY_TRACING_TRACEPARENT_LENGTH
#define GALAY_TRACING_TRACEPARENT_LENGTH 55u
#endif
/** @brief 字符串属性值最大长度，单位为字节。 */
#ifndef GALAY_TRACING_MAX_ATTRIBUTE_VALUE_LENGTH
#define GALAY_TRACING_MAX_ATTRIBUTE_VALUE_LENGTH 256u
#endif

#endif  /* GALAY_C_TRACING_MACRO_H */
