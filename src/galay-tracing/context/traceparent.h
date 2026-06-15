/**
 * @file traceparent.h
 * @brief W3C Trace Context traceparent/tracestate 解析与注入
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现 W3C Trace Context 规范中 traceparent 和 tracestate 头的
 * 解析（extract）与注入（inject）功能，用于跨服务边界传播追踪上下文。
 */

#pragma once

#include "galay-tracing/context/trace_context.h"

#include <expected>
#include <string>
#include <string_view>

namespace galay::tracing {

/**
 * @brief traceparent 解析错误类型
 */
enum class TraceparentError {
    kMalformed,              ///< 格式不正确
    kUnsupportedVersion,     ///< 不支持的版本号
    kInvalidTraceId,         ///< 无效的 TraceId（全零或格式错误）
    kInvalidSpanId,          ///< 无效的 SpanId（全零或格式错误）
    kInvalidFlags,           ///< 无效的标志位
};

/**
 * @brief 从 W3C traceparent 头解析追踪上下文
 * @details 解析 traceparent 字符串并保留 tracestate 作为不透明字符串。
 * traceparent 格式为：version-traceid-spanid-flags
 * @param value traceparent 头的值
 * @param tracestate tracestate 头的值（可选）
 * @return 解析成功返回 TraceContext，失败返回 TraceparentError
 */
[[nodiscard]] std::expected<TraceContext, TraceparentError> extractTraceparent(
    std::string_view value,
    std::string_view tracestate = {});

/**
 * @brief 将追踪上下文格式化为 W3C traceparent 头
 * @details 生成小写的 traceparent 字符串。若输入上下文无效则返回空字符串。
 * @param context 追踪上下文
 * @return traceparent 头字符串，上下文无效时返回空字符串
 */
[[nodiscard]] std::string injectTraceparent(const TraceContext& context);

/**
 * @brief 获取追踪上下文的 tracestate 值用于出站传播
 * @param context 追踪上下文
 * @return tracestate 不透明字符串
 */
[[nodiscard]] std::string injectTracestate(const TraceContext& context);

} // namespace galay::tracing
