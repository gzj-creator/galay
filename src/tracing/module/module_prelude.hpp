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

#include "tracing/common/source_location.h"
#include "tracing/common/span_id.h"
#include "tracing/common/trace_id.h"
#include "tracing/context/context_storage.h"
#include "tracing/context/trace_context.h"
#include "tracing/context/traceparent.h"
#include "tracing/kernel/batch_span_processor.h"
#include "tracing/kernel/file_span_exporter.h"
#include "tracing/kernel/otlp_http_exporter.h"
#include "tracing/kernel/sampler.h"
#include "tracing/kernel/span.h"
#include "tracing/kernel/span_guard.h"
#include "tracing/kernel/span_exporter.h"
#include "tracing/kernel/span_processor.h"
#include "tracing/log/console_sink.h"
#include "tracing/log/log_level.h"
#include "tracing/log/log_record.h"
#include "tracing/log/log_sink.h"
#include "tracing/log/logger.h"
