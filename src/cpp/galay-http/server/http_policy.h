/**
 * @file http_policy.h
 * @brief HTTP/1.1 生产级运行策略定义
 * @author galay-http
 * @version 1.0.0
 *
 * @details
 * 该文件只定义可组合的策略值类型，不直接改变读写、路由或代理行为。
 * 默认值保持与现有 `HttpReaderSetting` / `HttpWriterSetting` 兼容，
 * 后续任务会逐步把这些策略接入 route-mode 服务器、静态文件和反向代理路径。
 */

#ifndef GALAY_HTTP_POLICY_H
#define GALAY_HTTP_POLICY_H

#include "../protoc/http_base.h"

#include <chrono>
#include <cstddef>

namespace galay::http
{

/**
 * @brief HTTP 请求解析与接收限制。
 * @details
 * `max_header_size` 和 `max_body_size` 对齐现有 reader 默认限制。
 * 其他字段为 route-mode 的可选生产限制，默认 `0` 表示暂不启用，
 * 以保持当前行为兼容。
 */
struct HttpRequestLimits
{
    size_t max_header_size = DEFAULT_HTTP_MAX_HEADER_SIZE; ///< 请求头总字节上限。
    size_t max_body_size = DEFAULT_HTTP_MAX_BODY_SIZE;     ///< 请求体字节上限。
    size_t max_header_count = 0;                           ///< 最大 header 数量，0 表示不额外限制。
    size_t max_header_line_size = 0;                       ///< 单行 header 字节上限，0 表示不额外限制。
    size_t max_uri_size = 0;                               ///< URI 字节上限，0 表示沿用现有全局限制。
};

/**
 * @brief HTTP 响应写入限制。
 * @details 默认响应体上限与现有 `HttpWriterSetting` 保持一致。
 */
struct HttpResponseLimits
{
    size_t max_response_size = DEFAULT_HTTP_MAX_BODY_SIZE; ///< 响应总字节上限。
};

/**
 * @brief route-mode 请求读取与响应写入超时。
 * @details
 * 单位为毫秒，默认值对齐现有 reader/writer 超时。该结构不执行阻塞等待；
 * 后续接入时应通过协程 awaitable 的 `.timeout(...)` 使等待挂起而非阻塞线程。
 */
struct HttpTimeoutPolicy
{
    std::chrono::milliseconds request_header_timeout{DEFAULT_HTTP_RECV_TIME_MS}; ///< 请求头读取超时。
    std::chrono::milliseconds request_body_timeout{DEFAULT_HTTP_RECV_TIME_MS};   ///< 请求体读取超时。
    std::chrono::milliseconds response_write_timeout{DEFAULT_HTTP_SEND_TIME_MS}; ///< 响应写入超时。
};

/**
 * @brief Keep-Alive 生命周期策略。
 * @details 默认启用 HTTP/1.1 keep-alive，`0` 型限制表示不额外收紧当前行为。
 */
struct HttpKeepAlivePolicy
{
    bool enabled = true; ///< 是否允许 route-mode 使用 keep-alive。
    size_t max_requests_per_connection = 0; ///< 单连接最大请求数，0 表示不限制。
    std::chrono::milliseconds keep_alive_idle_timeout{DEFAULT_HTTP_KEEPALIVE_TIME_MS}; ///< 请求间空闲超时。
};

/**
 * @brief 反向代理默认策略。
 * @details
 * Task 1 仅暴露配置面；代理池、超时、健康状态和流式背压在后续任务中接入。
 */
struct HttpProxyPolicy
{
    size_t max_idle_connections_per_upstream = 32; ///< 每个上游最多保留的空闲连接数。
    std::chrono::milliseconds idle_ttl{DEFAULT_HTTP_KEEPALIVE_TIME_MS}; ///< 空闲连接保留时间。
    std::chrono::milliseconds connect_timeout{DEFAULT_HTTP_RECV_TIME_MS}; ///< 上游连接超时。
    std::chrono::milliseconds upstream_write_timeout{DEFAULT_HTTP_SEND_TIME_MS}; ///< 写上游超时。
    std::chrono::milliseconds upstream_read_timeout{DEFAULT_HTTP_RECV_TIME_MS}; ///< 读上游超时。
    std::chrono::milliseconds downstream_write_timeout{DEFAULT_HTTP_SEND_TIME_MS}; ///< 写下游超时。
    bool retry_stale_pooled_connection = true; ///< 是否保留现有 stale pooled connection 一次重试语义。
};

/**
 * @brief 静态文件服务默认策略。
 * @details 当前仅作为聚合策略的一部分保存，具体强制逻辑由后续静态文件加固任务接入。
 */
struct HttpStaticPolicy
{
    bool follow_symlinks = false;              ///< 是否允许跟随符号链接。
    size_t max_range_count = 0;                ///< 最大 Range 数，0 表示不额外限制。
    size_t max_aggregate_range_bytes = 0;      ///< 多 Range 聚合字节上限，0 表示不额外限制。
    bool reject_encoded_path_separators = true; ///< 是否拒绝编码后的路径分隔符。
    bool head_sends_body = false;              ///< HEAD 是否发送 body，默认不发送。
};

/**
 * @brief HTTP/1.1 route-mode 聚合生产策略。
 * @details
 * 该类型可放入 `HttpServerConfig` 或 `HttpRouter`，用于后续任务在读、写、代理、
 * 静态文件、keep-alive 和 TLS 边界中读取一致的默认值。
 */
struct HttpServerPolicy
{
    HttpRequestLimits request_limits;   ///< 请求解析和接收限制。
    HttpResponseLimits response_limits; ///< 响应写入限制。
    HttpTimeoutPolicy timeouts;         ///< 请求/响应超时策略。
    HttpKeepAlivePolicy keep_alive;     ///< Keep-Alive 生命周期策略。
    HttpProxyPolicy proxy;              ///< 反向代理默认策略。
    HttpStaticPolicy static_files;      ///< 静态文件默认策略。
};

} // namespace galay::http

#endif // GALAY_HTTP_POLICY_H
