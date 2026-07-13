/**
 * @file rpc_call.h
 * @brief RPC单次调用选项
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 定义调用级deadline、重试提示和metadata配置。该类型仅保存选项，
 *          不执行计时、取消或重试逻辑。
 */

#ifndef GALAY_RPC_CALL_H
#define GALAY_RPC_CALL_H

#include "rpc_metadata.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <atomic>

namespace galay::rpc
{

using RpcClock = std::chrono::steady_clock;  ///< RPC deadline使用的单调时钟

/**
 * @brief 取消回调注册句柄
 * @details 由 RpcCancellationToken::registerCallback 创建；调用方持有该句柄，
 *          在 pending 完成后调用 deactivate()，避免后续 cancel() 重复通知。
 */
class RpcCancellationRegistration {
public:
    explicit RpcCancellationRegistration(std::function<void()> callback)
        : m_callback(std::move(callback))
    {
    }

    RpcCancellationRegistration(const RpcCancellationRegistration&) = delete;
    RpcCancellationRegistration& operator=(const RpcCancellationRegistration&) = delete;
    RpcCancellationRegistration(RpcCancellationRegistration&&) = delete;
    RpcCancellationRegistration& operator=(RpcCancellationRegistration&&) = delete;

    /// @brief 停用注册，后续 cancel() 将跳过该回调。
    void deactivate() noexcept {
        m_active.store(false, std::memory_order_release);
    }

    /// @brief 如果仍处于活动状态则执行回调。
    bool notifyIfActive() const {
        if (!m_active.load(std::memory_order_acquire)) {
            return false;
        }
        if (!m_callback) {
            return false;
        }
        m_callback();
        return true;
    }

    std::shared_ptr<RpcCancellationRegistration> next; ///< 单向回调链

private:
    std::function<void()> m_callback; ///< 取消通知回调
    std::atomic<bool> m_active{true}; ///< 是否仍然有效
};

struct RpcCancellationState {
    /**
     * @brief 取消回调链表头
     * @note 通过 shared_ptr 原子自由函数访问，兼容未实现 atomic<shared_ptr> 特化的 Apple libc++，
     *       并在 cancel() 遍历期间保持 registration 生命周期。
     */
    std::shared_ptr<RpcCancellationRegistration> callbacks;
    std::atomic<bool> cancelled{false};
};

/**
 * @brief 取消令牌占位
 *
 * @details 当前仓库未提供统一的协程取消token，因此先保留轻量占位类型用于API兼容。
 *          后续接入仓库原生取消能力时可在不改变RpcCallOptions主结构的情况下扩展。
 */
class RpcCancellationToken {
public:
    RpcCancellationToken() = default;

    /// @brief 是否已经请求取消
    bool cancelled() const {
        return m_state && m_state->cancelled.load(std::memory_order_acquire);
    }

    /**
     * @brief 注册取消通知回调
     * @param callback cancel() 后执行的回调；不得阻塞
     * @return 注册句柄；空 token 返回 nullptr
     */
    std::shared_ptr<RpcCancellationRegistration> registerCallback(std::function<void()> callback) const {
        if (!m_state) {
            return nullptr;
        }

        auto registration = std::make_shared<RpcCancellationRegistration>(std::move(callback));
        auto head = std::atomic_load_explicit(
            &m_state->callbacks, std::memory_order_acquire);
        while (true) {
            std::shared_ptr<RpcCancellationRegistration> next = head;
            registration->next.swap(next);
            if (std::atomic_compare_exchange_weak_explicit(
                    &m_state->callbacks,
                    &head,
                    registration,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                break;
            }
        }

        if (cancelled()) {
            const bool notified = registration->notifyIfActive();
            if (!notified) {
                registration->deactivate();
            }
        }
        return registration;
    }

private:
    friend class RpcCancellationSource;
    explicit RpcCancellationToken(std::shared_ptr<RpcCancellationState> state)
        : m_state(std::move(state))
    {
    }

    std::shared_ptr<RpcCancellationState> m_state;  ///< 共享取消状态
};

/**
 * @brief RPC取消源
 *
 * @details 可在调用前或调用过程中请求取消。取消状态是无锁原子标志，检查不会阻塞。
 */
class RpcCancellationSource {
public:
    RpcCancellationSource()
        : m_state(std::make_shared<RpcCancellationState>())
    {
    }

    /// @brief 请求取消
    void cancel() const {
        const bool already_cancelled = m_state->cancelled.exchange(true, std::memory_order_acq_rel);
        if (already_cancelled) {
            return;
        }

        auto registration = std::atomic_load_explicit(
            &m_state->callbacks, std::memory_order_acquire);
        while (registration) {
            const bool did_notify = registration->notifyIfActive();
            if (!did_notify) {
                registration->deactivate();
            }
            std::shared_ptr<RpcCancellationRegistration> next = registration->next;
            registration.swap(next);
        }
    }

    /// @brief 获取传递给RpcCallOptions的token
    RpcCancellationToken token() const { return RpcCancellationToken(m_state); }

private:
    std::shared_ptr<RpcCancellationState> m_state;  ///< 共享取消状态
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
    RpcCallOptions& clearTimeout() {
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
    RpcCallOptions& clearDeadline() {
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
    std::optional<TimePoint> effectiveDeadline(TimePoint now) const {
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
    RpcCallOptions& maxAttempts(uint32_t value) {
        m_max_attempts = value;
        return *this;
    }

    /// @brief 清除最大尝试次数覆盖值
    RpcCallOptions& clearMaxAttempts() {
        m_max_attempts.reset();
        return *this;
    }

    /// @brief 获取最大尝试次数覆盖值
    std::optional<uint32_t> maxAttempts() const { return m_max_attempts; }

    /// @brief 获取可变metadata
    RpcMetadata& metadata() { return m_metadata; }
    /// @brief 获取只读metadata
    const RpcMetadata& metadata() const { return m_metadata; }

    /// @brief 设置取消token
    RpcCallOptions& cancellationToken(RpcCancellationToken token) {
        m_cancellation_token = std::move(token);
        return *this;
    }

    /// @brief 获取取消token
    std::optional<RpcCancellationToken> cancellationToken() const {
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
