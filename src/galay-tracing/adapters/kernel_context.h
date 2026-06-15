/**
 * @file kernel_context.h
 * @brief galay-kernel 回调与协程之间的追踪上下文桥接工具
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 提供 RAII 作用域守卫和上下文快照函数，用于将线程本地追踪上下文
 * 安全地传递到非挂载的 kernel 回调以及协程任务中。
 */

#pragma once

#include "galay-tracing/context/context_storage.h"

#include <optional>
#include <utility>

namespace galay::tracing {

/**
 * @brief 非挂载 kernel 回调的 RAII 追踪上下文作用域守卫
 * @details 在构造时将给定的上下文设置为当前线程的活跃上下文，
 * 析构时自动恢复之前的上下文。禁止跨 co_await/co_yield 持有此对象；
 * 应将捕获的上下文作为参数显式传递。
 * @note 不可复制
 */
class KernelTraceContextScope {
public:
    /**
     * @brief 构造并设置当前线程的追踪上下文
     * @param context 要设置的追踪上下文，可为空以清除当前上下文
     */
    explicit KernelTraceContextScope(std::optional<TraceContext> context)
        : m_previous(currentContext()) {
        setCurrentContext(std::move(context));
    }

    /**
     * @brief 析构时恢复之前的追踪上下文
     */
    ~KernelTraceContextScope() noexcept {
        try {
            setCurrentContext(std::move(m_previous));
        } catch (...) {
            clearCurrentContext();
        }
    }

    KernelTraceContextScope(const KernelTraceContextScope&) = delete;
    KernelTraceContextScope& operator=(const KernelTraceContextScope&) = delete;

private:
    std::optional<TraceContext> m_previous; ///< 保存之前的追踪上下文，析构时恢复
};

/**
 * @brief 快照当前线程的追踪上下文
 * @details 在将工作提交到 galay-kernel 之前调用此函数获取当前线程的追踪上下文快照。
 * 协程任务应将返回值作为参数传递，并使用 *_CTX 系列日志 API。
 * @return 当前线程的活跃追踪上下文，若无则返回空
 */
[[nodiscard]] inline std::optional<TraceContext> captureTraceContext() noexcept {
    return currentContext();
}

} // namespace galay::tracing
