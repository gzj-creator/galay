/**
 * @file file_span_exporter.cc
 * @brief 基于文件的 Span 导出器实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 将 Span 以 JSON Lines 格式追加写入本地文件，
 * 内部使用互斥锁保证线程安全的批量写入和文件刷新。
 */

#include "galay-tracing/kernel/file_span_exporter.h"

#include <span>
#include <string>

namespace galay::tracing {

namespace {

void appendJsonString(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    out.push_back('"');
}

void appendAttributeValue(std::string& out, const SpanAttributeValue& value) {
    switch (value.type()) {
    case SpanAttributeType::kInt64:
        out.append(std::to_string(value.asInt64()));
        break;
    case SpanAttributeType::kUInt64:
        out.append(std::to_string(value.asUInt64()));
        break;
    case SpanAttributeType::kDouble:
        out.append(std::to_string(value.asDouble()));
        break;
    case SpanAttributeType::kBool:
        out.append(value.asBool() ? "true" : "false");
        break;
    case SpanAttributeType::kString:
        appendJsonString(out, value.asString());
        break;
    }
}

void appendAttributes(std::string& out, std::span<const SpanAttribute> attributes) {
    out.push_back('[');
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out.append("{\"key\":");
        appendJsonString(out, attributes[i].name);
        out.append(",\"value\":");
        appendAttributeValue(out, attributes[i].value);
        out.push_back('}');
    }
    out.push_back(']');
}

void appendEvents(std::string& out, std::span<const SpanEvent> events) {
    out.append(",\"events\":[");
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out.append("{\"name\":");
        appendJsonString(out, events[i].name);
        out.append(",\"attributes\":");
        appendAttributes(out, events[i].attributes);
        out.push_back('}');
    }
    out.push_back(']');
}

void appendLinks(std::string& out, std::span<const SpanLink> links) {
    out.append(",\"links\":[");
    for (std::size_t i = 0; i < links.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out.append("{\"trace_id\":\"");
        out.append(links[i].context.traceId().toHex());
        out.append("\",\"span_id\":\"");
        out.append(links[i].context.spanId().toHex());
        out.append("\",\"attributes\":");
        appendAttributes(out, links[i].attributes);
        out.push_back('}');
    }
    out.push_back(']');
}

[[nodiscard]] std::string renderSpanJson(const Span& span) {
    const auto& context = span.spanContext();
    std::string line;
    line.append("{\"name\":");
    appendJsonString(line, span.name());
    line.append(",\"trace_id\":\"");
    line.append(context.traceId().toHex());
    line.append("\",\"span_id\":\"");
    line.append(context.spanId().toHex());
    line.append("\",\"sampled\":");
    line.append(context.sampled() ? "true" : "false");
    if (!span.events().empty()) {
        appendEvents(line, span.events());
    }
    if (!span.links().empty()) {
        appendLinks(line, span.links());
    }
    line.push_back('}');
    return line;
}

} // namespace

FileSpanExporter::FileSpanExporter(const std::filesystem::path& path)
    : m_out(path, std::ios::out | std::ios::app) {
}

ExportResult FileSpanExporter::exportSpans(std::span<const Span> spans) {
    std::lock_guard lock(m_mutex);
    if (!m_out) {
        return ExportResult::kFailure;
    }

    for (const auto& span : spans) {
        m_out << renderSpanJson(span) << '\n';
    }
    return m_out ? ExportResult::kSuccess : ExportResult::kFailure;
}

bool FileSpanExporter::forceFlush(std::chrono::milliseconds) {
    std::lock_guard lock(m_mutex);
    m_out.flush();
    return static_cast<bool>(m_out);
}

bool FileSpanExporter::shutdown(std::chrono::milliseconds timeout) {
    static_cast<void>(timeout);
    std::lock_guard lock(m_mutex);
    m_out.flush();
    m_out.close();
    return true;
}

} // namespace galay::tracing
