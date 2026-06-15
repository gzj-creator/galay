/**
 * @file otlp_http_exporter.h
 * @brief OTLP/HTTP JSON 格式的 Span 导出器
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现基于 OpenTelemetry Protocol (OTLP) HTTP JSON 格式的 Span 导出器，
 * 支持自定义 HTTP 传输层。可选启用基于 galay-http 协程的内置传输实现。
 */

#pragma once

#include "galay-tracing/kernel/span_exporter.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::tracing {

/**
 * @brief OTLP HTTP 请求头
 */
struct OtlpHttpHeader {
    std::string name;   ///< 请求头名称
    std::string value;  ///< 请求头值
};

/**
 * @brief 遥测仪表作用域配置
 */
struct InstrumentationScopeConfig {
    std::string name{"galay-tracing"}; ///< 仪表作用域名称
    std::string version;               ///< 仪表作用域版本
};

/**
 * @brief OTLP/HTTP JSON 导出器配置
 */
struct OtlpHttpExporterConfig {
    std::string endpoint{"http://127.0.0.1:4318/v1/traces"}; ///< OTLP 接收端地址
    std::chrono::milliseconds timeout{std::chrono::milliseconds(500)}; ///< 请求超时时间
    std::vector<OtlpHttpHeader> headers;         ///< 自定义 HTTP 请求头
    std::vector<SpanAttribute> resource_attributes; ///< 资源属性列表
    InstrumentationScopeConfig scope;            ///< 仪表作用域配置
};

/**
 * @brief OTLP HTTP 请求参数
 * @details 借用的视图仅在传输调用期间有效
 */
struct OtlpHttpRequest {
    std::string_view method{"POST"}; ///< HTTP 方法
    std::string_view endpoint;       ///< 请求端点 URL
    std::chrono::milliseconds timeout{std::chrono::milliseconds(500)}; ///< 请求超时
    std::span<const OtlpHttpHeader> headers; ///< HTTP 请求头视图
    std::string body;                ///< 请求体（JSON）
};

/**
 * @brief OTLP HTTP 响应
 */
struct OtlpHttpResponse {
    int status_code{0};  ///< HTTP 状态码
    std::string body;    ///< 响应体
    std::string error;   ///< 错误信息
};

/**
 * @brief OTLP HTTP 传输函数类型
 * @details 接收请求参数，返回响应的可调用对象
 */
using OtlpHttpTransport = std::function<OtlpHttpResponse(OtlpHttpRequest request)>;

/**
 * @brief galay-http 传输运行时配置
 */
struct GalayHttpOtlpTransportConfig {
    std::size_t io_scheduler_count{1};     ///< IO 调度器数量
    bool reject_on_runtime_thread{true};   ///< 是否拒绝在调度器线程上调用
};

#if defined(GALAY_TRACING_ENABLE_OTLP_HTTP)
/**
 * @brief 创建基于 galay-http 协程的同步 OTLP 传输
 * @details 在 galay-http 协程之上构建同步的 SpanExporter 传输层。
 * 调用仅阻塞调用线程；默认情况下来自调度器线程的调用会快速失败。
 * @param config 传输运行时配置
 * @return OtlpHttpTransport 可调用对象
 */
OtlpHttpTransport makeGalayHttpOtlpTransport(GalayHttpOtlpTransportConfig config = {});
#endif

/**
 * @brief OTLP/HTTP JSON Span 导出器
 * @details 将已采样的 Span 序列化为 OTLP JSON 格式并通过 HTTP POST
 * 发送到 OTLP 接收端。启用 GALAY_TRACING_ENABLE_OTLP_HTTP 时默认使用 galay-http 传输。
 */
class OtlpHttpExporter final : public SpanExporter {
public:
    /**
     * @brief 使用默认传输构造导出器
     * @param config 导出器配置
     */
    explicit OtlpHttpExporter(OtlpHttpExporterConfig config = {});

    /**
     * @brief 使用自定义传输构造导出器
     * @param config 导出器配置
     * @param transport 自定义 HTTP 传输函数
     */
    OtlpHttpExporter(OtlpHttpExporterConfig config, OtlpHttpTransport transport);

    /**
     * @brief 导出一批 Span 到 OTLP 端点
     * @param spans 待导出的 Span 只读视图
     * @return 导出结果
     */
    ExportResult exportSpans(std::span<const Span> spans) override;

    /**
     * @brief 强制刷新（本实现为空操作）
     * @param timeout 超时时间
     * @return 始终返回 true
     */
    bool forceFlush(std::chrono::milliseconds timeout) override;

    /**
     * @brief 关闭导出器
     * @param timeout 超时时间
     * @return 始终返回 true
     */
    bool shutdown(std::chrono::milliseconds timeout) override;

private:
    OtlpHttpExporterConfig m_config;       ///< 导出器配置
    std::vector<OtlpHttpHeader> m_headers; ///< 编码后的请求头
    OtlpHttpTransport m_transport;         ///< HTTP 传输函数
};

} // namespace galay::tracing
