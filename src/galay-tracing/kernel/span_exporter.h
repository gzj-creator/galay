/**
 * @file span_exporter.h
 * @brief Span 导出器抽象基类
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 定义 SpanExporter 接口，负责将已结束的 Span 批量导出到后端
 * （如 OTLP、文件等）。用户可实现此接口对接自定义后端。
 */

#pragma once

#include "galay-tracing/kernel/span.h"

#include <chrono>
#include <span>

namespace galay::tracing {

/**
 * @brief Span 导出结果
 */
enum class ExportResult {
    kSuccess,  ///< 导出成功
    kFailure,  ///< 导出失败
};

/**
 * @brief Span 导出器抽象基类
 * @details 定义将 Span 数据导出到后端的接口。实现类负责具体的序列化和传输逻辑，
 * 例如 OTLP/HTTP、文件写入等。导出器由 SpanProcessor 调用。
 */
class SpanExporter {
public:
    virtual ~SpanExporter() = default;

    /**
     * @brief 导出一批 Span 到后端
     * @param spans 待导出的 Span 只读视图
     * @return 导出结果
     */
    virtual ExportResult exportSpans(std::span<const Span> spans) = 0;

    /**
     * @brief 强制刷新缓冲区中的所有 Span
     * @param timeout 超时时间
     * @return 成功刷新返回 true，超时或失败返回 false
     */
    virtual bool forceFlush(std::chrono::milliseconds timeout);

    /**
     * @brief 关闭导出器并释放资源
     * @param timeout 超时时间
     * @return 成功关闭返回 true，超时或失败返回 false
     */
    virtual bool shutdown(std::chrono::milliseconds timeout);
};

} // namespace galay::tracing
