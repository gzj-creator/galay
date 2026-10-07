/**
 * @file rpc_call.h
 * @brief RPC单次调用选项
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 定义调用级deadline、metadata和单 owner 取消域。取消状态由 source
 *          内嵌持有，token 借用，回调注册由调用协程按值持有。
 */

#ifndef GALAY_RPC_CALL_H
#define GALAY_RPC_CALL_H

#include "rpc_metadata.h"

#include <chrono>
#include <functional>
#include <optional>
#include <utility>

namespace galay::rpc
{

using RpcClock = std::chrono::steady_clock;  ///< RPC deadline使用的单调时钟

class RpcCancellationRegistration;

namespace detail {
struct RpcCancellationState {
    RpcCancellationRegistration* callbacks = nullptr;
    bool cancelled = false;
};
} // namespace detail

/**
 * @brief 单 owner 的 RAII 取消注册，析构或 deactivate() 时 O(1) 摘链。
 * @details 注册直接存放在调用方栈/协程帧中；移动时修复借用链，不分配注册节点。
 *          所有操作必须与 source.cancel() 在同一 owner 上串行执行。
 */
class RpcCancellationRegistration {
public:
    RpcCancellationRegistration() = default;
    ~RpcCancellationRegistration() { deactivate(); }

    RpcCancellationRegistration(RpcCancellationRegistration&& other) noexcept {
        take_from(other);
    }

    RpcCancellationRegistration& operator=(RpcCancellationRegistration&& other) noexcept {
        if (this != &other) {
            deactivate();
            take_from(other);
        }
        return *this;
    }

    /// @brief 摘除注册并释放回调捕获；可以重复调用。
    void deactivate() noexcept {
        unlink();
        m_callback = nullptr;
    }

private:
    friend class RpcCancellationToken;
    friend class RpcCancellationSource;
    RpcCancellationRegistration(const RpcCancellationRegistration&) = delete;
    RpcCancellationRegistration& operator=(const RpcCancellationRegistration&) = delete;

    RpcCancellationRegistration(detail::RpcCancellationState* state,
                                std::function<void()> callback)
        : m_callback(std::move(callback)) {
        if (state == nullptr || !m_callback) {
            m_callback = nullptr;
            return;
        }
        if (state->cancelled) {
            auto notify = std::move(m_callback);
            notify();
            return;
        }
        m_state = state;
        m_next = state->callbacks;
        if (m_next != nullptr) m_next->m_previous = this;
        state->callbacks = this;
    }

    void unlink() noexcept {
        if (m_state == nullptr) return;
        if (m_previous != nullptr) {
            m_previous->m_next = m_next;
        } else {
            m_state->callbacks = m_next;
        }
        if (m_next != nullptr) m_next->m_previous = m_previous;
        m_state = nullptr;
        m_previous = nullptr;
        m_next = nullptr;
    }

    void take_from(RpcCancellationRegistration& other) noexcept {
        m_callback = std::move(other.m_callback);
        m_state = std::exchange(other.m_state, nullptr);
        m_previous = std::exchange(other.m_previous, nullptr);
        m_next = std::exchange(other.m_next, nullptr);
        if (m_state == nullptr) return;
        if (m_previous != nullptr) {
            m_previous->m_next = this;
        } else {
            m_state->callbacks = this;
        }
        if (m_next != nullptr) m_next->m_previous = this;
    }

    std::function<void()> m_callback;
    detail::RpcCancellationState* m_state = nullptr;
    RpcCancellationRegistration* m_previous = nullptr;
    RpcCancellationRegistration* m_next = nullptr;
};

/**
 * @brief 借用 source 内嵌状态的取消令牌，可廉价复制。
 * @note source 必须覆盖 token 的所有使用。注册、查询、取消与注销必须在同一
 *       owner 上串行执行；外部线程应投递取消消息，不能直接访问该 token。
 */
class RpcCancellationToken {
public:
    RpcCancellationToken() = default;

    /// @brief 是否已经请求取消
    bool cancelled() const noexcept {
        return m_state != nullptr && m_state->cancelled;
    }

    /**
     * @brief 注册取消通知回调
     * @param callback owner 上同步执行的回调；不得阻塞、抛出或销毁 source。
     * @return 值注册；空 token 返回空注册，已取消的 token 立即执行回调。
     * @note 调用方必须持有返回值直到不再需要通知。
     */
    [[nodiscard]] RpcCancellationRegistration register_callback(std::function<void()> callback) const {
        return RpcCancellationRegistration(m_state, std::move(callback));
    }

private:
    friend class RpcCancellationSource;
    explicit RpcCancellationToken(detail::RpcCancellationState* state) noexcept
        : m_state(state)
    {
    }

    detail::RpcCancellationState* m_state = nullptr;  ///< 借用，绝不延长 source 生命周期
};

/**
 * @brief RPC取消源
 *
 * @details 单 owner、无内部同步。状态内嵌，无堆分配或引用计数。因 token 和注册
 *          借用其地址，source 不可复制/移动；必须活到所有调用完成。
 *          外部取消通过既有调度器/消息通道投递到 owner 后执行 cancel()。
 */
class RpcCancellationSource {
public:
    RpcCancellationSource() = default;
    RpcCancellationSource(RpcCancellationSource&&) = delete;
    RpcCancellationSource& operator=(RpcCancellationSource&&) = delete;

    ~RpcCancellationSource() {
        // 捕获对象的析构可能重入注册，不允许再把节点挂回正在销毁的域。
        m_state.cancelled = true;
        while (m_state.callbacks != nullptr) m_state.callbacks->deactivate();
    }

    /// @brief 请求取消
    void cancel() {
        if (m_state.cancelled) return;
        m_state.cancelled = true;
        while (m_state.callbacks != nullptr) {
            auto* registration = m_state.callbacks;
            registration->unlink();
            // 先摘链并取走回调，允许通知中删除自身、其他注册或重入 cancel()。
            auto callback = std::move(registration->m_callback);
            callback();
        }
    }

    /// @brief 获取传递给RpcCallOptions的token
    RpcCancellationToken token() noexcept { return RpcCancellationToken(&m_state); }

private:
    RpcCancellationSource(const RpcCancellationSource&) = delete;
    RpcCancellationSource& operator=(const RpcCancellationSource&) = delete;
    detail::RpcCancellationState m_state;
};

/**
 * @brief 单次RPC调用选项
 *
 * @details RpcCallOptions是值类型，可在发起调用前配置。成员函数不阻塞，也不启动后台
 *          任务；deadline计算由调用点传入当前时间完成。metadata由本对象持有。
 */
class RpcCallOptions {
public:
    using Duration = RpcClock::duration;
    using TimePoint = RpcClock::time_point;

    /// @brief 设置相对超时；当未设置绝对deadline时生效
    RpcCallOptions& timeout(Duration value) {
        m_timeout = value;
        return *this;
    }

    /// @brief 清除相对超时
    RpcCallOptions& clear_timeout() {
        m_timeout.reset();
        return *this;
    }

    /// @brief 获取相对超时
    std::optional<Duration> timeout() const { return m_timeout; }

    /// @brief 设置绝对deadline；同时存在timeout时优先使用deadline
    RpcCallOptions& deadline(TimePoint value) {
        m_deadline = value;
        return *this;
    }

    /// @brief 清除绝对deadline
    RpcCallOptions& clear_deadline() {
        m_deadline.reset();
        return *this;
    }

    /// @brief 获取绝对deadline
    std::optional<TimePoint> deadline() const { return m_deadline; }

    /**
     * @brief 计算最终deadline
     * @param now 调用发起时刻
     * @return 绝对deadline；未配置deadline/timeout时为空
     */
    std::optional<TimePoint> effective_deadline(TimePoint now) const {
        if (m_deadline.has_value()) {
            return m_deadline;
        }
        if (m_timeout.has_value()) {
            return now + *m_timeout;
        }
        return std::nullopt;
    }

    /// @brief 设置调用是否可按策略重试
    RpcCallOptions& idempotent(bool value) {
        m_idempotent = value;
        return *this;
    }

    /// @brief 调用是否可按策略重试
    bool idempotent() const { return m_idempotent; }

    /// @brief 设置最大尝试次数覆盖值
    RpcCallOptions& max_attempts(uint32_t value) {
        m_max_attempts = value;
        return *this;
    }

    /// @brief 清除最大尝试次数覆盖值
    RpcCallOptions& clear_max_attempts() {
        m_max_attempts.reset();
        return *this;
    }

    /// @brief 获取最大尝试次数覆盖值
    std::optional<uint32_t> max_attempts() const { return m_max_attempts; }

    /// @brief 获取可变metadata
    RpcMetadata& metadata() { return m_metadata; }
    /// @brief 获取只读metadata
    const RpcMetadata& metadata() const { return m_metadata; }

    /// @brief 设置取消token
    RpcCallOptions& cancellation_token(RpcCancellationToken token) {
        m_cancellation_token = std::move(token);
        return *this;
    }

    /// @brief 获取取消token
    std::optional<RpcCancellationToken> cancellation_token() const {
        return m_cancellation_token;
    }

private:
    std::optional<Duration> m_timeout;  ///< 相对超时
    std::optional<TimePoint> m_deadline;  ///< 绝对deadline
    bool m_idempotent = false;  ///< 是否幂等
    std::optional<uint32_t> m_max_attempts;  ///< 最大尝试次数覆盖
    RpcMetadata m_metadata;  ///< 调用级metadata
    std::optional<RpcCancellationToken> m_cancellation_token;  ///< 取消token
};

}  // namespace galay::rpc

#endif  // GALAY_RPC_CALL_H
