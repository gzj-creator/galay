/**
 * @file module_prelude.hpp
 * @brief galay-tracing 模块统一引入头文件
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 一次性引入 galay-tracing 库的所有公共头文件，
 * 方便用户通过单个 #include 使用完整的追踪功能。
 * 包括：通用类型、上下文管理、Span 核心、处理器/导出器、采样器和日志系统。
 */

#pragma once

#include "../common/source_location.h"
#include "../common/span_id.h"
#include "../common/trace_id.h"
#include "../adapters/http_headers.h"
#include "../context/context_storage.h"
#include "../context/trace_context.h"
#include "../context/traceparent.h"
#include "../kernel/batch_span_processor.h"
#include "../kernel/file_span_exporter.h"
#include "../kernel/otlp_http_exporter.h"
#include "../kernel/sampler.h"
#include "../kernel/span.h"
#include "../kernel/span_guard.h"
#include "../kernel/span_exporter.h"
#include "../kernel/span_processor.h"
#include "../kernel/tracer_provider.h"
#include "../log/console_sink.h"
#include "../log/log_level.h"
#include "../log/log_record.h"
#include "../log/log_sink.h"
#include "../log/logger.h"
