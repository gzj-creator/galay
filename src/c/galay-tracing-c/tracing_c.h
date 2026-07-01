/**
 * @file tracing_c.h
 * @brief galay-tracing 模块的 C ABI 声明
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 本文件暴露分布式追踪的 C 接口，覆盖 TraceId/SpanId、
 * W3C traceparent/tracestate 传播、Provider/Tracer/Span 生命周期、
 * 采样器以及同步文件日志/Span 导出能力。
 *
 * C ABI 约定：
 * - 必填输入字符串均以 `(const char*, size_t)` 表示，不要求以 NUL 结尾；
 *   当长度非 0 时指针必须非空；可选字符串的空指针语义见各参数说明。
 * - 固定长度输出（TraceId、SpanId、traceparent）只写入协议字节，
 *   不追加 NUL；`GALAY_TRACING_*_LENGTH` 常量均不包含 NUL。
 * - `written` 表示所需或实际写入的字节数，不包含 NUL；格式化 API
 *   在能够计算长度且 `written` 非空时，即使缓冲区不足也会写入所需长度。
 * - 由本 ABI 创建或解析得到的 opaque handle 归调用方所有，必须使用
 *   对应 `*_destroy` 释放；destroy 接口对空指针和已置空 handle 幂等。
 * - Provider 拥有导出器/处理器；Tracer 和 Span 只借用 Provider，调用方
 *   必须保证 Provider 生命周期覆盖其创建的 Tracer、Span 以及 flush/shutdown。
 * - 接口均为同步调用，不返回 Task/协程对象，也不会挂起协程；文件 exporter
 *   和 logger 会在调用线程执行文件 I/O、flush、close 和互斥锁等待，可能阻塞。
 * - 除同步文件 exporter/logger 内部写文件使用互斥锁保护外，同一 opaque handle
 *   的并发读写不保证线程安全；跨线程共享 handle 时调用方负责外部同步。
 * - 所有可恢复错误通过 `galay_status_t` 显式返回，常见错误包括
 *   `GALAY_INVALID_ARGUMENT`、`GALAY_PROTOCOL_ERROR`、`GALAY_OUT_OF_MEMORY`、
 *   `GALAY_IO_ERROR` 和 `GALAY_INTERNAL_ERROR`。
 */
#ifndef GALAY_C_TRACING_TRACING_C_H
#define GALAY_C_TRACING_TRACING_C_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TraceId 十六进制文本长度。
 * @details 固定为 32 个小写十六进制字符，不包含 NUL 终止符。
 */
#define GALAY_TRACING_TRACE_ID_HEX_LENGTH 32u

/**
 * @brief SpanId 十六进制文本长度。
 * @details 固定为 16 个小写十六进制字符，不包含 NUL 终止符。
 */
#define GALAY_TRACING_SPAN_ID_HEX_LENGTH 16u

/**
 * @brief W3C traceparent 文本长度。
 * @details 当前 ABI 生成版本为 `00` 的 traceparent，固定 55 字节，不包含 NUL 终止符。
 */
#define GALAY_TRACING_TRACEPARENT_LENGTH 55u

/**
 * @brief 字符串属性值最大长度。
 * @details 仅约束 `GALAY_TRACING_ATTRIBUTE_STRING` 的 `value_len`，单位为字节，不包含 NUL。
 */
#define GALAY_TRACING_MAX_ATTRIBUTE_VALUE_LENGTH 256u

/**
 * @brief C ABI TraceId 原始字节表示。
 * @details 16 字节（128 位）追踪标识符。全零值无效；随机生成和解析接口保证返回有效值。
 */
typedef struct galay_tracing_trace_id_t { uint8_t bytes[16]; } galay_tracing_trace_id_t;

/**
 * @brief C ABI SpanId 原始字节表示。
 * @details 8 字节（64 位）Span 标识符。全零值无效；随机生成和解析接口保证返回有效值。
 */
typedef struct galay_tracing_span_id_t { uint8_t bytes[8]; } galay_tracing_span_id_t;

/**
 * @brief opaque 追踪传播上下文。
 * @details 由 traceparent 解析/提取接口创建，包含 TraceId、SpanId、flags 和 tracestate。
 * 调用方通过 `galay_tracing_trace_context_destroy` 释放。
 */
typedef struct galay_tracing_trace_context_t galay_tracing_trace_context_t;

/**
 * @brief opaque Tracing Provider。
 * @details Provider 拥有 SpanProcessor/exporter 状态。Tracer 和 Span 仅借用 Provider，
 * 因此 Provider 必须最后销毁。
 */
typedef struct galay_tracing_provider_t galay_tracing_provider_t;

/**
 * @brief opaque Tracer。
 * @details 由 Provider 创建，用于启动 Span。Tracer 不拥有 Provider。
 */
typedef struct galay_tracing_tracer_t galay_tracing_tracer_t;

/**
 * @brief opaque Span。
 * @details Span 持有当前操作的上下文、属性、事件、链接和结束状态。成功 end 后不可再修改，
 * 但仍需调用 `galay_tracing_span_destroy` 释放 handle。
 */
typedef struct galay_tracing_span_t galay_tracing_span_t;

/**
 * @brief opaque 采样器。
 * @details 由 `galay_tracing_sampler_create` 创建，用于按父上下文/TraceId 判断是否采样。
 */
typedef struct galay_tracing_sampler_t galay_tracing_sampler_t;

/**
 * @brief opaque 文件 logger。
 * @details 当前 C ABI 提供同步文件 logger；写日志在调用线程执行文件 I/O，可能阻塞。
 */
typedef struct galay_tracing_logger_t galay_tracing_logger_t;

/**
 * @brief Span 属性值类型。
 * @details `name` 字段对所有类型必填；只有 string 类型读取 `value/value_len`，
 * 其他类型分别读取对应数值字段。
 */
typedef enum galay_tracing_attribute_type_t {
    GALAY_TRACING_ATTRIBUTE_STRING = 0, ///< 字符串属性，读取 `value/value_len`。
    GALAY_TRACING_ATTRIBUTE_INT64 = 1,  ///< 64 位有符号整数属性，读取 `int64_value`。
    GALAY_TRACING_ATTRIBUTE_UINT64 = 2, ///< 64 位无符号整数属性，读取 `uint64_value`。
    GALAY_TRACING_ATTRIBUTE_DOUBLE = 3, ///< 双精度浮点属性，读取 `double_value`。
    GALAY_TRACING_ATTRIBUTE_BOOL = 4    ///< 布尔属性，读取 `bool_value`。
} galay_tracing_attribute_type_t;

/**
 * @brief Span 状态码。
 * @details 用于标记 Span 结果。未设置是默认状态；OK 表示成功；ERROR 表示失败。
 */
typedef enum galay_tracing_span_status_code_t {
    GALAY_TRACING_SPAN_STATUS_UNSET = 0, ///< 未设置状态。
    GALAY_TRACING_SPAN_STATUS_OK = 1,    ///< 操作成功。
    GALAY_TRACING_SPAN_STATUS_ERROR = 2  ///< 操作失败。
} galay_tracing_span_status_code_t;

/**
 * @brief 采样器类型。
 * @details ratio 仅对 `GALAY_TRACING_SAMPLER_TRACE_ID_RATIO` 有意义，范围为 `[0.0, 1.0]`。
 */
typedef enum galay_tracing_sampler_kind_t {
    GALAY_TRACING_SAMPLER_ALWAYS_ON = 0,       ///< 总是采样。
    GALAY_TRACING_SAMPLER_ALWAYS_OFF = 1,      ///< 总是不采样。
    GALAY_TRACING_SAMPLER_TRACE_ID_RATIO = 2   ///< 按 TraceId 比例采样。
} galay_tracing_sampler_kind_t;

/**
 * @brief Logger 日志级别。
 * @details logger 仅写入大于等于创建时配置级别的记录；OFF 关闭输出。
 */
typedef enum galay_tracing_log_level_t {
    GALAY_TRACING_LOG_TRACE = 0, ///< 最细粒度追踪日志。
    GALAY_TRACING_LOG_DEBUG = 1, ///< 调试日志。
    GALAY_TRACING_LOG_INFO = 2,  ///< 普通信息日志。
    GALAY_TRACING_LOG_WARN = 3,  ///< 警告日志。
    GALAY_TRACING_LOG_ERROR = 4, ///< 错误日志。
    GALAY_TRACING_LOG_OFF = 5    ///< 关闭日志输出。
} galay_tracing_log_level_t;

/**
 * @brief Span 属性输入结构。
 * @details 该结构只在调用期间被读取；实现会复制属性名和字符串属性值，
 * 调用返回后调用方可以释放输入缓冲区。`name` 必须非空且 `name_len > 0`。
 */
typedef struct galay_tracing_attribute_t {
    const char* name;                       ///< 属性名缓冲区，不要求 NUL 结尾。
    size_t name_len;                        ///< 属性名长度，单位为字节，必须大于 0。
    const char* value;                      ///< 字符串属性值缓冲区；非字符串类型忽略。
    size_t value_len;                       ///< 字符串属性值长度，单位为字节，不包含 NUL。
    galay_tracing_attribute_type_t type;    ///< 属性值类型。
    int64_t int64_value;                    ///< int64 属性值。
    uint64_t uint64_value;                  ///< uint64 属性值。
    double double_value;                    ///< double 属性值。
    galay_bool_t bool_value;                ///< bool 属性值，`GALAY_TRUE` 表示 true。
} galay_tracing_attribute_t;

/**
 * @brief 获取 tracing C ABI 的错误字符串。
 * @param status `galay_status_t` 状态码。
 * @return 指向静态错误字符串的只读指针，调用方不得释放。
 */
const char* galay_tracing_get_error(galay_status_t status);

/**
 * @brief 随机生成 TraceId。
 * @param out 输出 TraceId 指针，必须非空。
 * @return 成功返回 `GALAY_OK`；`out` 为空返回 `GALAY_INVALID_ARGUMENT`。
 * @note 生成结果为非全零有效 TraceId。该调用同步执行，不追加 NUL。
 */
galay_status_t galay_tracing_trace_id_generate(galay_tracing_trace_id_t* out);

/**
 * @brief 检查 TraceId 是否有效。
 * @param id 待检查 TraceId 指针，可为空。
 * @return 非空且非全零时返回 `GALAY_TRUE`，否则返回 `GALAY_FALSE`。
 */
galay_bool_t galay_tracing_trace_id_is_valid(const galay_tracing_trace_id_t* id);

/**
 * @brief 将 TraceId 格式化为 32 字节小写十六进制文本。
 * @param id 输入 TraceId 指针，必须非空。
 * @param out 输出缓冲区，必须非空且至少 `GALAY_TRACING_TRACE_ID_HEX_LENGTH` 字节。
 * @param out_len 输出缓冲区长度。
 * @param written 输出实际/所需字节数，必须非空；不包含 NUL。
 * @return 成功返回 `GALAY_OK`；参数为空或缓冲区不足返回 `GALAY_INVALID_ARGUMENT`。
 * @note 输出不追加 NUL；调用方如需 C 字符串需自行额外分配并补 NUL。
 */
galay_status_t galay_tracing_trace_id_format(const galay_tracing_trace_id_t* id, char* out,
                                             size_t out_len, size_t* written);

/**
 * @brief 从 32 字节十六进制文本解析 TraceId。
 * @param data 输入文本缓冲区，必须非空，不要求 NUL 结尾。
 * @param data_len 输入长度，必须等于 `GALAY_TRACING_TRACE_ID_HEX_LENGTH`。
 * @param out 输出 TraceId 指针，必须非空。
 * @return 成功返回 `GALAY_OK`；长度错误、格式错误、全零值或空指针返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_tracing_trace_id_parse(const char* data, size_t data_len,
                                            galay_tracing_trace_id_t* out);

/**
 * @brief 随机生成 SpanId。
 * @param out 输出 SpanId 指针，必须非空。
 * @return 成功返回 `GALAY_OK`；`out` 为空返回 `GALAY_INVALID_ARGUMENT`。
 * @note 生成结果为非全零有效 SpanId。
 */
galay_status_t galay_tracing_span_id_generate(galay_tracing_span_id_t* out);

/**
 * @brief 检查 SpanId 是否有效。
 * @param id 待检查 SpanId 指针，可为空。
 * @return 非空且非全零时返回 `GALAY_TRUE`，否则返回 `GALAY_FALSE`。
 */
galay_bool_t galay_tracing_span_id_is_valid(const galay_tracing_span_id_t* id);

/**
 * @brief 将 SpanId 格式化为 16 字节小写十六进制文本。
 * @param id 输入 SpanId 指针，必须非空。
 * @param out 输出缓冲区，必须非空且至少 `GALAY_TRACING_SPAN_ID_HEX_LENGTH` 字节。
 * @param out_len 输出缓冲区长度。
 * @param written 输出实际/所需字节数，必须非空；不包含 NUL。
 * @return 成功返回 `GALAY_OK`；参数为空或缓冲区不足返回 `GALAY_INVALID_ARGUMENT`。
 * @note 输出不追加 NUL；调用方如需 C 字符串需自行额外分配并补 NUL。
 */
galay_status_t galay_tracing_span_id_format(const galay_tracing_span_id_t* id, char* out,
                                            size_t out_len, size_t* written);

/**
 * @brief 从 16 字节十六进制文本解析 SpanId。
 * @param data 输入文本缓冲区，必须非空，不要求 NUL 结尾。
 * @param data_len 输入长度，必须等于 `GALAY_TRACING_SPAN_ID_HEX_LENGTH`。
 * @param out 输出 SpanId 指针，必须非空。
 * @return 成功返回 `GALAY_OK`；长度错误、格式错误、全零值或空指针返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_tracing_span_id_parse(const char* data, size_t data_len,
                                           galay_tracing_span_id_t* out);

/**
 * @brief 解析 W3C traceparent/tracestate 为追踪上下文。
 * @param data traceparent 缓冲区，必须非空，不要求 NUL 结尾。
 * @param data_len traceparent 长度，通常为 `GALAY_TRACING_TRACEPARENT_LENGTH`。
 * @param tracestate 可选 tracestate 缓冲区；为空时 `tracestate_len` 应为 0。
 * @param tracestate_len tracestate 长度，不包含 NUL。
 * @param out 输出上下文 handle 指针，必须非空；成功后由调用方 destroy。
 * @return 成功返回 `GALAY_OK`；traceparent 缺失、`out` 缺失或协议格式无效返回
 * `GALAY_PROTOCOL_ERROR`；内存不足返回 `GALAY_OUT_OF_MEMORY`。
 * @note 这是 `galay_tracing_trace_context_extract` 的别名语义。
 */
galay_status_t galay_tracing_traceparent_parse(const char* data, size_t data_len,
                                               const char* tracestate, size_t tracestate_len,
                                               galay_tracing_trace_context_t** out);

/**
 * @brief 将追踪上下文格式化为 W3C traceparent。
 * @param context 输入上下文 handle，必须非空且有效。
 * @param out 输出缓冲区，必须非空且至少 `GALAY_TRACING_TRACEPARENT_LENGTH` 字节。
 * @param out_len 输出缓冲区长度。
 * @param written 输出实际/所需字节数，必须非空；不包含 NUL。
 * @return 成功返回 `GALAY_OK`；上下文无效、参数为空或缓冲区不足返回 `GALAY_INVALID_ARGUMENT`。
 * @note 输出固定 55 字节，不追加 NUL。
 */
galay_status_t galay_tracing_traceparent_format(const galay_tracing_trace_context_t* context,
                                                char* out, size_t out_len, size_t* written);

/**
 * @brief 从传播头提取追踪上下文。
 * @param traceparent traceparent 缓冲区，必须非空，不要求 NUL 结尾。
 * @param traceparent_len traceparent 长度。
 * @param tracestate 可选 tracestate 缓冲区；为空时按空 tracestate 处理。
 * @param tracestate_len tracestate 长度，不包含 NUL。
 * @param out 输出上下文 handle 指针，必须非空；成功后由调用方 destroy。
 * @return 成功返回 `GALAY_OK`；traceparent 缺失、`out` 缺失或格式无效返回
 * `GALAY_PROTOCOL_ERROR`；内存不足返回 `GALAY_OUT_OF_MEMORY`。
 * @note 当 `out` 非空时，失败路径会先将 `*out` 置为 NULL，避免调用方误用旧 handle。
 */
galay_status_t galay_tracing_trace_context_extract(const char* traceparent, size_t traceparent_len,
                                                   const char* tracestate, size_t tracestate_len,
                                                   galay_tracing_trace_context_t** out);

/**
 * @brief 将追踪上下文注入为 traceparent/tracestate 文本。
 * @param context 输入上下文 handle，必须非空且有效。
 * @param traceparent_out traceparent 输出缓冲区，必须非空。
 * @param traceparent_out_len traceparent 输出缓冲区长度，至少 `GALAY_TRACING_TRACEPARENT_LENGTH`。
 * @param traceparent_written traceparent 实际/所需字节数，必须非空；不包含 NUL。
 * @param tracestate_out tracestate 输出缓冲区，必须非空；即使 tracestate 为空也需提供。
 * @param tracestate_out_len tracestate 输出缓冲区长度。
 * @param tracestate_written tracestate 实际/所需字节数，必须非空；不包含 NUL。
 * @return 成功返回 `GALAY_OK`；上下文无效、参数为空或任一缓冲区不足返回
 * `GALAY_INVALID_ARGUMENT`。
 * @note 两个输出均不追加 NUL。若 traceparent 写入失败，函数会直接返回且不会写入 tracestate。
 */
galay_status_t galay_tracing_trace_context_inject(const galay_tracing_trace_context_t* context,
                                                  char* traceparent_out, size_t traceparent_out_len,
                                                  size_t* traceparent_written,
                                                  char* tracestate_out, size_t tracestate_out_len,
                                                  size_t* tracestate_written);

/**
 * @brief 销毁追踪上下文 handle。
 * @param context 指向上下文 handle 的指针，可为空；`*context` 可为空。
 * @note 幂等操作；成功销毁后会将 `*context` 置为 NULL。
 */
void galay_tracing_trace_context_destroy(galay_tracing_trace_context_t** context);

/**
 * @brief 读取追踪上下文 flags。
 * @param context 输入上下文 handle，必须非空。
 * @param flags 输出 flags 指针，必须非空。
 * @return 成功返回 `GALAY_OK`；参数为空返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_tracing_trace_context_flags(const galay_tracing_trace_context_t* context,
                                                 uint8_t* flags);

/**
 * @brief 创建 Tracing Provider。
 * @param out 输出 Provider handle 指针，必须非空；成功后由调用方 destroy。
 * @return 成功返回 `GALAY_OK`；`out` 为空返回 `GALAY_INVALID_ARGUMENT`；
 * 内存不足返回 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_tracing_provider_create(galay_tracing_provider_t** out);

/**
 * @brief 销毁 Tracing Provider。
 * @param provider 指向 Provider handle 的指针，可为空；`*provider` 可为空。
 * @note 幂等操作；成功销毁后会将 `*provider` 置为 NULL。调用方必须先销毁或停止使用
 * 由该 Provider 创建的 Tracer/Span。
 */
void galay_tracing_provider_destroy(galay_tracing_provider_t** provider);

/**
 * @brief 为 Provider 设置同步文件 Span exporter。
 * @param provider Provider handle，必须非空。
 * @param path 文件路径缓冲区，必须非空且 `path_len > 0`，不要求 NUL 结尾。
 * @param path_len 文件路径长度。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；
 * 内存不足返回 `GALAY_OUT_OF_MEMORY`。
 * @note exporter 追加写 JSON Lines；Span end、force_flush、shutdown 会在调用线程执行文件 I/O，
 * 可能阻塞。重复设置会替换旧 exporter。文件打开失败不会在本函数中单独返回 `GALAY_IO_ERROR`，
 * 后续 force_flush 可能返回 `GALAY_IO_ERROR`。
 */
galay_status_t galay_tracing_provider_set_file_exporter(galay_tracing_provider_t* provider,
                                                        const char* path, size_t path_len);

/**
 * @brief 同步刷新 Provider exporter。
 * @param provider Provider handle，必须非空且已设置 exporter。
 * @param timeout_ms 超时时间，单位毫秒；负值表示无限等待。当前同步文件 exporter 会忽略该值并立即 flush。
 * @return 成功返回 `GALAY_OK`；Provider/exporter 缺失返回 `GALAY_INVALID_ARGUMENT`；
 * flush 失败返回 `GALAY_IO_ERROR`。
 * @note 该调用可能阻塞调用线程。
 */
galay_status_t galay_tracing_provider_force_flush(galay_tracing_provider_t* provider,
                                                  int64_t timeout_ms);

/**
 * @brief 同步关闭 Provider exporter。
 * @param provider Provider handle，必须非空且已设置 exporter。
 * @param timeout_ms 超时时间，单位毫秒；负值表示无限等待。当前同步文件 exporter 会忽略该值并立即关闭。
 * @return 成功返回 `GALAY_OK`；Provider/exporter 缺失返回 `GALAY_INVALID_ARGUMENT`；
 * shutdown 失败返回 `GALAY_IO_ERROR`。
 * @note 该调用可能阻塞调用线程；shutdown 后继续 end Span 不会再写入该文件 exporter。
 */
galay_status_t galay_tracing_provider_shutdown(galay_tracing_provider_t* provider,
                                               int64_t timeout_ms);

/**
 * @brief 从 Provider 创建 Tracer。
 * @param provider Provider handle，必须非空，并且生命周期必须覆盖 Tracer。
 * @param name Tracer 名称缓冲区，必须非空且 `name_len > 0`，不要求 NUL 结尾。
 * @param name_len Tracer 名称长度。
 * @param out 输出 Tracer handle 指针，必须非空；成功后由调用方 destroy。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；
 * 内存不足返回 `GALAY_OUT_OF_MEMORY`。
 * @note 实现会复制 `name`，调用返回后调用方可释放输入缓冲区。
 */
galay_status_t galay_tracing_tracer_create(galay_tracing_provider_t* provider, const char* name,
                                           size_t name_len, galay_tracing_tracer_t** out);

/**
 * @brief 销毁 Tracer。
 * @param tracer 指向 Tracer handle 的指针，可为空；`*tracer` 可为空。
 * @note 幂等操作；成功销毁后会将 `*tracer` 置为 NULL。销毁 Tracer 不会销毁 Provider 或已创建的 Span。
 */
void galay_tracing_tracer_destroy(galay_tracing_tracer_t** tracer);

/**
 * @brief 启动子 Span。
 * @param tracer Tracer handle，必须非空。
 * @param name Span 名称缓冲区，必须非空且 `name_len > 0`，不要求 NUL 结尾。
 * @param name_len Span 名称长度。
 * @param context 父追踪上下文，必须非空且有效。
 * @param out 输出 Span handle 指针，必须非空；成功后由调用方 end/destroy。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；
 * 内存不足返回 `GALAY_OUT_OF_MEMORY`。
 * @note 新 Span 继承父 TraceId、flags 和 tracestate，生成新的 SpanId，并记录父 SpanId。
 * Span 借用 Tracer 的 Provider，Provider 必须覆盖 Span 生命周期。
 */
galay_status_t galay_tracing_tracer_start_span(galay_tracing_tracer_t* tracer, const char* name,
                                               size_t name_len,
                                               const galay_tracing_trace_context_t* context,
                                               galay_tracing_span_t** out);

/**
 * @brief 销毁 Span handle。
 * @param span 指向 Span handle 的指针，可为空；`*span` 可为空。
 * @note 幂等操作；成功销毁后会将 `*span` 置为 NULL。未 end 的 Span 被销毁时不会自动导出。
 */
void galay_tracing_span_destroy(galay_tracing_span_t** span);

/**
 * @brief 向 Span 添加事件。
 * @param span Span handle，必须非空且尚未 end。
 * @param name 事件名称缓冲区，必须非空且 `name_len > 0`，不要求 NUL 结尾。
 * @param name_len 事件名称长度。
 * @param attributes 可选属性数组；`attribute_count == 0` 时可为空。
 * @param attribute_count 属性数量。
 * @return 成功返回 `GALAY_OK`；参数无效、Span 已 end、属性无效或字符串属性过长返回
 * `GALAY_INVALID_ARGUMENT`；底层写入失败返回 `GALAY_INTERNAL_ERROR`。
 * @note 实现会复制事件名、属性名和字符串属性值。
 */
galay_status_t galay_tracing_span_add_event(galay_tracing_span_t* span, const char* name,
                                            size_t name_len,
                                            const galay_tracing_attribute_t* attributes,
                                            size_t attribute_count);

/**
 * @brief 设置或覆盖 Span 属性。
 * @param span Span handle，必须非空且尚未 end。
 * @param attribute 属性输入指针，必须非空且内容有效。
 * @return 成功返回 `GALAY_OK`；参数无效、Span 已 end 或字符串属性过长返回
 * `GALAY_INVALID_ARGUMENT`；底层写入失败返回 `GALAY_INTERNAL_ERROR`。
 * @note 实现会复制属性名和字符串属性值。
 */
galay_status_t galay_tracing_span_set_attribute(galay_tracing_span_t* span,
                                                const galay_tracing_attribute_t* attribute);

/**
 * @brief 设置 Span 状态。
 * @param span Span handle，必须非空且尚未 end。
 * @param code 状态码，必须是 `galay_tracing_span_status_code_t` 的有效值。
 * @param message 可选状态消息；当 `message_len > 0` 时必须非空。
 * @param message_len 状态消息长度，不包含 NUL。
 * @return 成功返回 `GALAY_OK`；参数无效或 Span 已 end 返回 `GALAY_INVALID_ARGUMENT`。
 * @note 实现会复制状态消息。
 */
galay_status_t galay_tracing_span_set_status(galay_tracing_span_t* span,
                                             galay_tracing_span_status_code_t code,
                                             const char* message, size_t message_len);

/**
 * @brief 为 Span 添加链接。
 * @param span Span handle，必须非空且尚未 end。
 * @param context 被链接的追踪上下文，必须非空且有效。
 * @param attributes 可选链接属性数组；`attribute_count == 0` 时可为空。
 * @param attribute_count 属性数量。
 * @return 成功返回 `GALAY_OK`；参数无效、Span 已 end、属性无效或字符串属性过长返回
 * `GALAY_INVALID_ARGUMENT`；底层写入失败返回 `GALAY_INTERNAL_ERROR`。
 * @note 实现会复制链接属性，并保留被链接上下文的 TraceId、SpanId、flags 和 tracestate。
 */
galay_status_t galay_tracing_span_add_link(galay_tracing_span_t* span,
                                           const galay_tracing_trace_context_t* context,
                                           const galay_tracing_attribute_t* attributes,
                                           size_t attribute_count);

/**
 * @brief 获取 Span 属性数量。
 * @param span Span handle，必须非空。
 * @param out 输出数量指针，必须非空。
 * @return 成功返回 `GALAY_OK`；参数为空返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_tracing_span_attribute_count(const galay_tracing_span_t* span, size_t* out);

/**
 * @brief 获取 Span 事件数量。
 * @param span Span handle，必须非空。
 * @param out 输出数量指针，必须非空。
 * @return 成功返回 `GALAY_OK`；参数为空返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_tracing_span_event_count(const galay_tracing_span_t* span, size_t* out);

/**
 * @brief 获取 Span 链接数量。
 * @param span Span handle，必须非空。
 * @param out 输出数量指针，必须非空。
 * @return 成功返回 `GALAY_OK`；参数为空返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_tracing_span_link_count(const galay_tracing_span_t* span, size_t* out);

/**
 * @brief 获取 Span 当前状态码。
 * @param span Span handle，必须非空。
 * @param out 输出状态码指针，必须非空。
 * @return 成功返回 `GALAY_OK`；参数为空返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_tracing_span_status(const galay_tracing_span_t* span,
                                         galay_tracing_span_status_code_t* out);

/**
 * @brief 结束 Span 并触发 Provider exporter。
 * @param span Span handle，必须非空且尚未 end。
 * @return 成功返回 `GALAY_OK`；参数为空、Span 已 end 或内部 Span 缺失返回 `GALAY_INVALID_ARGUMENT`。
 * @note 成功后 Span 进入 ended 状态，后续修改 API 会失败；如 Provider 设置了同步文件 exporter，
 * 该调用会在调用线程渲染并写入 JSON Lines，可能阻塞。没有 exporter 时仍返回成功。
 */
galay_status_t galay_tracing_span_end(galay_tracing_span_t* span);

/**
 * @brief 创建采样器。
 * @param kind 采样器类型，必须是 `galay_tracing_sampler_kind_t` 的有效值。
 * @param ratio TraceId 比例采样阈值，仅 `GALAY_TRACING_SAMPLER_TRACE_ID_RATIO` 使用，范围 `[0.0, 1.0]`。
 * @param out 输出 Sampler handle 指针，必须非空；成功后由调用方 destroy。
 * @return 成功返回 `GALAY_OK`；`out` 为空或 ratio 越界返回 `GALAY_INVALID_ARGUMENT`；
 * 内存不足返回 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_tracing_sampler_create(galay_tracing_sampler_kind_t kind, double ratio,
                                            galay_tracing_sampler_t** out);

/**
 * @brief 销毁采样器。
 * @param sampler 指向 Sampler handle 的指针，可为空；`*sampler` 可为空。
 * @note 幂等操作；成功销毁后会将 `*sampler` 置为 NULL。
 */
void galay_tracing_sampler_destroy(galay_tracing_sampler_t** sampler);

/**
 * @brief 判断给定上下文是否应采样。
 * @param sampler Sampler handle，必须非空。
 * @param context 追踪上下文，必须非空且有效。
 * @param out 输出布尔值指针，必须非空。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_tracing_sampler_should_sample(const galay_tracing_sampler_t* sampler,
                                                   const galay_tracing_trace_context_t* context,
                                                   galay_bool_t* out);

/**
 * @brief 创建同步文件 logger。
 * @param path 文件路径缓冲区，必须非空且 `path_len > 0`，不要求 NUL 结尾。
 * @param path_len 文件路径长度。
 * @param level 最低输出日志级别，必须是 `galay_tracing_log_level_t` 的有效值。
 * @param out 输出 Logger handle 指针，必须非空；成功后由调用方 destroy。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；
 * 内存不足返回 `GALAY_OUT_OF_MEMORY`。
 * @note logger 追加写文件并在写入后 flush；日志调用可能阻塞调用线程。文件打开/写入失败
 * 当前不通过 `galay_tracing_logger_log` 的返回值暴露。
 */
galay_status_t galay_tracing_logger_create_file(const char* path, size_t path_len,
                                                galay_tracing_log_level_t level,
                                                galay_tracing_logger_t** out);

/**
 * @brief 写入一条日志。
 * @param logger Logger handle，必须非空。
 * @param level 当前记录级别，必须是 `galay_tracing_log_level_t` 的有效值。
 * @param context 可选追踪上下文；为空或无效时日志不携带 trace/span id。
 * @param message 日志消息缓冲区，必须非空且 `message_len > 0`，不要求 NUL 结尾。
 * @param message_len 日志消息长度。
 * @return 成功或级别被过滤均返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note 启用输出时会同步写文件并 flush，可能阻塞调用线程；实现会复制 message。
 */
galay_status_t galay_tracing_logger_log(galay_tracing_logger_t* logger,
                                        galay_tracing_log_level_t level,
                                        const galay_tracing_trace_context_t* context,
                                        const char* message, size_t message_len);

/**
 * @brief 销毁 logger。
 * @param logger 指向 Logger handle 的指针，可为空；`*logger` 可为空。
 * @note 幂等操作；成功销毁后会将 `*logger` 置为 NULL。
 */
void galay_tracing_logger_destroy(galay_tracing_logger_t** logger);

#ifdef __cplusplus
}
#endif

#endif
