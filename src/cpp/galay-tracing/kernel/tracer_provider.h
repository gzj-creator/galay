/**
 * @file tracer_provider.h
 * @brief 追踪处理器的进程级访问入口
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 提供轻量级 SpanProcessor 作用域配置，供测试和嵌入式场景临时
 * 接入 Span 结束后的处理器。该头文件不拥有处理器生命周期，调用方须保证
 * SpanProcessor 在作用域内保持有效。
 */

#pragma once

#include "../context/context_storage.h"
#include "span_processor.h"

#include <atomic>

namespace galay::tracing {

namespace detail {

inline std::atomic<SpanProcessor*> g_currentSpanProcessor{nullptr};

} // namespace detail

/**
 * @brief 设置当前进程级 SpanProcessor
 * @details 不获取所有权。传入 nullptr 表示不提交结束的 Span。
 * @param processor SpanProcessor 指针，调用方负责保证生命周期
 */
inline void setSpanProcessor(SpanProcessor* processor) noexcept {
    detail::g_currentSpanProcessor.store(processor, std::memory_order_release);
}

/**
 * @brief 获取当前进程级 SpanProcessor
 * @return 当前 SpanProcessor 指针；未配置时返回 nullptr
 */
[[nodiscard]] inline SpanProcessor* currentSpanProcessor() noexcept {
    return detail::g_currentSpanProcessor.load(std::memory_order_acquire);
}

/**
 * @brief 临时替换当前 SpanProcessor 的 RAII 作用域
 * @details 构造时保存旧处理器并设置新处理器，析构时恢复旧处理器。
 * 该类型不拥有 SpanProcessor。
 */
class SpanProcessorScope {
public:
    /**
     * @brief 构造作用域并替换当前处理器
     * @param processor 新的 SpanProcessor 指针，nullptr 表示清空
     */
    explicit SpanProcessorScope(SpanProcessor* processor) noexcept
        : m_previous(currentSpanProcessor()) {
        setSpanProcessor(processor);
    }

    /**
     * @brief 析构时恢复进入作用域前的处理器
     */
    ~SpanProcessorScope() {
        setSpanProcessor(m_previous);
    }

    SpanProcessorScope(const SpanProcessorScope&) = delete;
    SpanProcessorScope& operator=(const SpanProcessorScope&) = delete;

private:
    SpanProcessor* m_previous{nullptr}; ///< 进入作用域前的处理器，不拥有所有权
};

} // namespace galay::tracing
