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

#include "galay-tracing/module/module_prelude.hpp"

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
using ::galay::tracing::SpanExporter;
using ::galay::tracing::SpanGuard;
using ::galay::tracing::SpanId;
using ::galay::tracing::SpanKind;
using ::galay::tracing::SpanProcessor;
using ::galay::tracing::SpanStatus;
using ::galay::tracing::SpanStatusCode;
using ::galay::tracing::SpanTimingPolicy;
using ::galay::tracing::StructuredLogRecord;
using ::galay::tracing::StructuredLogWriter;
using ::galay::tracing::TraceContext;
using ::galay::tracing::TraceId;
using ::galay::tracing::TraceIdRatioSampler;
using ::galay::tracing::TraceparentError;
using ::galay::tracing::clearCurrentContext;
using ::galay::tracing::currentContext;
using ::galay::tracing::currentSampler;
using ::galay::tracing::defaultLogger;
using ::galay::tracing::extractTraceparent;
using ::galay::tracing::event;
using ::galay::tracing::field;
using ::galay::tracing::injectTraceparent;
using ::galay::tracing::injectTracestate;
using ::galay::tracing::logDebug;
using ::galay::tracing::logDebugAt;
using ::galay::tracing::logError;
using ::galay::tracing::logErrorAt;
using ::galay::tracing::logInfo;
using ::galay::tracing::logInfoAt;
using ::galay::tracing::logLevelName;
using ::galay::tracing::logTrace;
using ::galay::tracing::logTraceAt;
using ::galay::tracing::logWarn;
using ::galay::tracing::logWarnAt;
using ::galay::tracing::makeLogContext;
using ::galay::tracing::setCurrentContext;
using ::galay::tracing::setDefaultLogger;
using ::galay::tracing::setSampler;
using ::galay::tracing::setSpanTimingPolicy;
using ::galay::tracing::spanAttribute;
using ::galay::tracing::spanTimingPolicy;
using ::galay::tracing::startServerSpan;
using ::galay::tracing::startSpan;
#if defined(GALAY_TRACING_ENABLE_OTLP_HTTP)
using ::galay::tracing::GalayHttpOtlpTransportConfig;
using ::galay::tracing::makeGalayHttpOtlpTransport;
#endif
} // namespace galay::tracing
