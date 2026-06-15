/**
 * @file span_processor.h
 * @brief Span 处理器抽象基类
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 定义 SpanProcessor 接口，负责接收已结束的 Span 并决定
 * 如何处理（如批量缓冲、直接导出等）。处理器在 Span 生命周期结束时被调用。
 */

#pragma once

#include "galay-tracing/kernel/span.h"

#include <chrono>

namespace galay::tracing {

/**
 * @brief Span 处理器抽象基类
 * @details 定义 Span 结束后的处理接口。实现类可提供批处理、过滤、
 * 重试等策略。典型实现包括 SimpleSpanProcessor 和 BatchSpanProcessor。
 */
class SpanProcessor {
public:
    virtual ~SpanProcessor() = default;

    /**
     * @brief 处理已结束的 Span
     * @param span 已结束的 Span（调用方移交所有权，处理器可异步保存）
     */
    virtual void onEnd(Span&& span) = 0;

    /**
     * @brief 强制刷新所有待处理的 Span
     * @param timeout 超时时间
     * @return 成功刷新返回 true，超时或失败返回 false
     */
    virtual bool forceFlush(std::chrono::milliseconds timeout) = 0;

    /**
     * @brief 关闭处理器并释放资源
     * @param timeout 超时时间
     * @return 成功关闭返回 true，超时或失败返回 false
     */
    virtual bool shutdown(std::chrono::milliseconds timeout) = 0;
};

} // namespace galay::tracing
