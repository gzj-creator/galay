/**
 * @file galay_tracing.cppm
 * @brief galay-tracing C++20 模块定义
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 将 galay-tracing 库的所有公共类型和函数导出为 C++20 命名模块
 * galay.tracing，包括：追踪标识符、上下文管理、Span 核心类型、
 * 处理器/导出器、采样器、日志系统及便捷函数。
 */

module;

#include "module_prelude.hpp"

// 本模块头：显式在全局片段中包含（prelude 只承担外部头，见
// scripts/common/106_gen_module_prelude.py 的不变量），名称经下方 using 列表导出。
#include "../adapters/http_headers.h"
#include "../common/source_location.h"
#include "../common/span_id.h"
#include "../common/trace_id.h"
#include "../common/tracing_log.h"
#include "../context/context_storage.h"
#include "../context/trace_context.h"
#include "../context/traceparent.h"
#include "../kernel/batch_span_processor.h"
#include "../kernel/file_span_exporter.h"
#include "../kernel/otlp_http_exporter.h"
#include "../kernel/sampler.h"
#include "../kernel/span_exporter.h"
#include "../kernel/span_guard.h"
#include "../kernel/span.h"
#include "../kernel/span_processor.h"
#include "../kernel/tracer_provider.h"
#include "../log/console_sink.h"
#include "../log/logger.h"
#include "../log/log_level.h"
#include "../log/log_record.h"
#include "../log/log_sink.h"

export module galay.tracing;

export namespace galay::tracing {
using ::galay::tracing::AlwaysOffSampler;
using ::galay::tracing::AlwaysOnSampler;
using ::galay::tracing::BatchSpanProcessor;
using ::galay::tracing::BatchSpanProcessorConfig;
using ::galay::tracing::BatchSpanScheduleMode;
using ::galay::tracing::ConsoleSink;
using ::galay::tracing::ExportResult;
using ::galay::tracing::FileSpanExporter;
using ::galay::tracing::InstrumentationScopeConfig;
using ::galay::tracing::LogLevel;
using ::galay::tracing::LogField;
using ::galay::tracing::LogFieldType;
using ::galay::tracing::LogFieldValue;
using ::galay::tracing::LogContext;
using ::galay::tracing::LogRecord;
using ::galay::tracing::LogSink;
using ::galay::tracing::Logger;
using ::galay::tracing::OtlpHttpExporter;
using ::galay::tracing::OtlpHttpExporterConfig;
using ::galay::tracing::OtlpHttpHeader;
using ::galay::tracing::OtlpHttpRequest;
using ::galay::tracing::OtlpHttpResponse;
using ::galay::tracing::OtlpHttpTransport;
using ::galay::tracing::ParentBasedSampler;
using ::galay::tracing::Sampler;
using ::galay::tracing::SourceLocation;
using ::galay::tracing::Span;
using ::galay::tracing::SpanAttribute;
using ::galay::tracing::SpanAttributeType;
using ::galay::tracing::SpanAttributeValue;
using ::galay::tracing::SpanContext;
using ::galay::tracing::SpanEvent;
using ::galay::tracing::SpanExporter;
using ::galay::tracing::SpanGuard;
using ::galay::tracing::SpanId;
using ::galay::tracing::SpanKind;
using ::galay::tracing::SpanLink;
using ::galay::tracing::SpanProcessor;
using ::galay::tracing::SpanProcessorScope;
using ::galay::tracing::SpanStatus;
using ::galay::tracing::SpanStatusCode;
using ::galay::tracing::SpanTimingPolicy;
using ::galay::tracing::StructuredLogRecord;
using ::galay::tracing::StructuredLogWriter;
using ::galay::tracing::TraceContext;
using ::galay::tracing::TraceHeaderGetter;
using ::galay::tracing::TraceHeaderSetter;
using ::galay::tracing::TraceId;
using ::galay::tracing::TraceIdRatioSampler;
using ::galay::tracing::TraceparentError;
using ::galay::tracing::clear_current_context;
using ::galay::tracing::current_context;
using ::galay::tracing::current_sampler;
using ::galay::tracing::current_span_processor;
using ::galay::tracing::default_logger;
using ::galay::tracing::extract_trace_context_from_headers;
using ::galay::tracing::extract_traceparent;
using ::galay::tracing::event;
using ::galay::tracing::field;
using ::galay::tracing::inject_trace_context_to_headers;
using ::galay::tracing::inject_traceparent;
using ::galay::tracing::inject_tracestate;
using ::galay::tracing::log;
using ::galay::tracing::log_debug;
using ::galay::tracing::log_debug_at;
using ::galay::tracing::log_error;
using ::galay::tracing::log_error_at;
using ::galay::tracing::log_info;
using ::galay::tracing::log_info_at;
using ::galay::tracing::log_level_name;
using ::galay::tracing::log_trace;
using ::galay::tracing::log_trace_at;
using ::galay::tracing::log_warn;
using ::galay::tracing::log_warn_at;
using ::galay::tracing::make_log_context;
using ::galay::tracing::set_current_context;
using ::galay::tracing::set_default_logger;
using ::galay::tracing::set_sampler;
using ::galay::tracing::set_span_processor;
using ::galay::tracing::set_span_timing_policy;
using ::galay::tracing::span_attribute;
using ::galay::tracing::span_timing_policy;
using ::galay::tracing::start_server_span;
using ::galay::tracing::start_span;
#if defined(GALAY_TRACING_ENABLE_OTLP_HTTP)
using ::galay::tracing::GalayHttpOtlpTransportConfig;
using ::galay::tracing::make_galay_http_otlp_transport;
#endif
} // namespace galay::tracing
