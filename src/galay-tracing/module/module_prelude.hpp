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

#include "galay-tracing/common/source_location.h"
#include "galay-tracing/common/span_id.h"
#include "galay-tracing/common/trace_id.h"
#include "galay-tracing/context/context_storage.h"
#include "galay-tracing/context/trace_context.h"
#include "galay-tracing/context/traceparent.h"
#include "galay-tracing/kernel/batch_span_processor.h"
#include "galay-tracing/kernel/file_span_exporter.h"
#include "galay-tracing/kernel/otlp_http_exporter.h"
#include "galay-tracing/kernel/sampler.h"
#include "galay-tracing/kernel/span.h"
#include "galay-tracing/kernel/span_guard.h"
#include "galay-tracing/kernel/span_exporter.h"
#include "galay-tracing/kernel/span_processor.h"
#include "galay-tracing/log/console_sink.h"
#include "galay-tracing/log/log_level.h"
#include "galay-tracing/log/log_record.h"
#include "galay-tracing/log/log_sink.h"
#include "galay-tracing/log/logger.h"
