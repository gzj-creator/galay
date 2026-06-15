/**
 * @file file_span_exporter.h
 * @brief 基于文件的 Span 导出器
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 将 Span 数据以 JSON Lines 格式写入本地文件，
 * 适用于调试、开发和离线分析场景。
 */

#pragma once

#include "galay-tracing/kernel/span_exporter.h"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace galay::tracing {

/**
 * @brief 文件 Span 导出器
 * @details 将 Span 以 JSON Lines 格式追加写入指定文件路径。
 * 内部使用互斥锁保证线程安全。适用于调试、开发和离线分析场景。
 */
class FileSpanExporter final : public SpanExporter {
public:
    /**
     * @brief 构造文件导出器
     * @param path 输出文件路径
     */
    explicit FileSpanExporter(const std::filesystem::path& path);

    /**
     * @brief 将一批 Span 写入文件
     * @param spans 待导出的 Span 只读视图
     * @return 导出结果
     */
    ExportResult exportSpans(std::span<const Span> spans) override;

    /**
     * @brief 刷新文件输出流
     * @param timeout 超时时间（本实现忽略，立即刷新）
     * @return 成功返回 true
     */
    bool forceFlush(std::chrono::milliseconds timeout) override;

    /**
     * @brief 关闭文件输出流
     * @param timeout 超时时间（本实现忽略，立即关闭）
     * @return 成功返回 true
     */
    bool shutdown(std::chrono::milliseconds timeout) override;

private:
    std::mutex m_mutex;   ///< 文件写入互斥锁
    std::ofstream m_out;  ///< 文件输出流
};

} // namespace galay::tracing
