/**
 * @file console_sink.cc
 * @brief 控制台日志输出 Sink 实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 将 LogRecord 格式化为 key=value 形式并输出到指定的标准流，
 * 支持追踪上下文（trace_id/span_id）和源码位置信息。
 */

#include "galay-tracing/log/console_sink.h"

#include "galay-tracing/log/log_level.h"

#include <iostream>

namespace galay::tracing {

ConsoleSink::ConsoleSink(std::ostream& out)
    : m_out(&out) {
}

ConsoleSink::ConsoleSink()
    : ConsoleSink(std::cout) {
}

void ConsoleSink::write(const LogRecord& record) {
    auto& out = *m_out;
    out << "level=" << logLevelName(record.level);
    if (record.context.has_value()) {
        out << " trace_id=" << record.context->traceId().toHex() << " span_id=" << record.context->spanId().toHex();
    }
    out << " file=" << record.source.file << ':' << record.source.line << " msg=\"" << record.message << "\"\n";
}

} // namespace galay::tracing
