/**
 * @file circuit_breaker.hpp
 * @brief 无锁熔断器实现
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 使用原子变量实现熔断器状态管理，适合高并发场景和协程环境。
 *          支持 Closed -> Open -> HalfOpen 状态转换和带降级的执行模式。
 */

#ifndef GALAY_UTILS_CIRCUIT_BREAKER_HPP
#define GALAY_UTILS_CIRCUIT_BREAKER_HPP

#include "../common/defn.hpp"
#include <chrono>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <type_traits>
#include <utility>

namespace galay::utils {

/// 熔断器状态枚举
enum class CircuitState : int { Closed = 0, Open = 1, HalfOpen = 2 };

/// 熔断器自身产生的错误类型
enum class CircuitBreakerError { Open };

/**
 * @brief 熔断器执行接口接受的 expected-like 结果类型
 * @details 结果类型需要提供 value_type、error_type 和 has_value()，可直接适配
 *          std::expected<T, E>。execute() 在熔断打开时会构造 CircuitBreakerError::Open，
 *          因此其 error_type 还需要能从 CircuitBreakerError 构造。
 */
template<typename T>
concept CircuitBreakerExpected =
    !std::is_reference_v<T> &&
    requires(const std::remove_cvref_t<T>& result) {
        typename std::remove_cvref_t<T>::value_type;
        typename std::remove_cvref_t<T>::error_type;
        { result.has_value() } -> std::convertible_to<bool>;
    };

namespace detail {

template<typename T>
concept CircuitBreakerOpenExpected = CircuitBreakerExpected<T> && requires {
    requires std::constructible_from<typename std::remove_cvref_t<T>::error_type,
                                     CircuitBreakerError>;
    requires std::constructible_from<std::remove_cvref_t<T>,
                                     std::unexpected<typename std::remove_cvref_t<T>::error_type>>;
};

template<typename F>
concept CircuitBreakerOperation =
    std::invocable<F> &&
    CircuitBreakerOpenExpected<std::invoke_result_t<F>>;

template<typename F>
concept CircuitBreakerFallbackOperation =
    std::invocable<F> &&
    CircuitBreakerExpected<std::invoke_result_t<F>>;

template<typename F, typename Fallback>
concept CircuitBreakerFallbackPair =
    CircuitBreakerFallbackOperation<F> &&
    CircuitBreakerFallbackOperation<Fallback> &&
    std::same_as<std::invoke_result_t<F>, std::invoke_result_t<Fallback>>;

} // namespace detail

/**
 * @brief 熔断器配置结构体
 * @details 定义熔断器的失败阈值、成功阈值和重置超时时间。
 */
struct CircuitBreakerConfig {
    size_t failureThreshold = 5; ///< 触发熔断的连续失败次数阈值
    size_t successThreshold = 3; ///< 半开状态下恢复的连续成功次数阈值
    size_t halfOpenMaxRequests = 1; ///< 半开状态下允许并发探测的最大请求数
    std::chrono::seconds resetTimeout{30}; ///< 熔断后到半开状态的超时时间
};

/**
 * @brief 无锁熔断器实现
 * @tparam ClockType 时间源类型，需要提供 now() 和 time_point；默认使用 std::chrono::steady_clock
 *
 * 使用原子变量实现状态管理，适合高并发场景和协程环境。
 * BasicCircuitBreaker 可用于注入手动时钟做确定性测试；CircuitBreaker 是默认时钟别名。
 *
 * 状态转换：
 * - Closed -> Open: 连续失败次数达到阈值
 * - Open -> HalfOpen: 超时后自动转换
 * - HalfOpen -> Closed: 连续成功次数达到阈值
 * - HalfOpen -> Open: 发生失败
 */
template<typename ClockType = std::chrono::steady_clock>
class BasicCircuitBreaker {
public:
    using Clock = ClockType;
    using TimePoint = Clock::time_point;

    explicit BasicCircuitBreaker(CircuitBreakerConfig config = {})
        : m_config(normalizeConfig(config))
        , m_resetTimeoutNs(timeoutToNs(m_config.resetTimeout))
        , m_state(static_cast<int>(CircuitState::Closed))
        , m_failureCount(0)
        , m_successCount(0)
        , m_halfOpenInFlight(0)
        , m_lastFailureTimeNs(0) {}

    /**
     * @brief 判断是否允许请求通过
     * @return 允许请求返回 true，熔断中返回 false
     */
    bool allowRequest() {
        auto currentState = static_cast<CircuitState>(m_state.load(std::memory_order_acquire));

        switch (currentState) {
            case CircuitState::Closed:
                return true;

            case CircuitState::Open: {
                auto now = nowNs();
                auto lastFailure = m_lastFailureTimeNs.load(std::memory_order_acquire);
                auto timeoutNs = m_resetTimeoutNs;

                if (now >= lastFailure && now - lastFailure >= timeoutNs) {
                    // 尝试转换到 HalfOpen
                    int expected = static_cast<int>(CircuitState::Open);
                    if (m_state.compare_exchange_strong(expected,
                            static_cast<int>(CircuitState::HalfOpen),
                            std::memory_order_acq_rel)) {
                        m_successCount.store(0, std::memory_order_release);
                        m_failureCount.store(0, std::memory_order_release);
                        m_halfOpenInFlight.store(0, std::memory_order_release);
                    }

                    auto observedState = static_cast<CircuitState>(
                        m_state.load(std::memory_order_acquire));
                    if (observedState == CircuitState::HalfOpen) {
                        return tryAcquireHalfOpenProbe();
                    }
                    return observedState == CircuitState::Closed;
                }
                return false;
            }

            case CircuitState::HalfOpen:
                return tryAcquireHalfOpenProbe();
        }

        return false;
    }

    /**
     * @brief 记录一次成功调用
     */
    void onSuccess() {
        auto currentState = static_cast<CircuitState>(m_state.load(std::memory_order_acquire));

        switch (currentState) {
            case CircuitState::Closed:
                if (m_failureCount.load(std::memory_order_relaxed) != 0) {
                    m_failureCount.store(0, std::memory_order_relaxed);
                }
                break;

            case CircuitState::HalfOpen: {
                releaseHalfOpenProbe();
                auto count = m_successCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count >= m_config.successThreshold) {
                    // 尝试转换到 Closed
                    int expected = static_cast<int>(CircuitState::HalfOpen);
                    if (m_state.compare_exchange_strong(expected,
                            static_cast<int>(CircuitState::Closed),
                            std::memory_order_acq_rel)) {
                        m_failureCount.store(0, std::memory_order_relaxed);
                        m_successCount.store(0, std::memory_order_relaxed);
                        m_halfOpenInFlight.store(0, std::memory_order_relaxed);
                    }
                }
                break;
            }

            case CircuitState::Open:
                break;
        }
    }

    /**
     * @brief 记录一次失败调用
     */
    void onFailure() {
        auto now = nowNs();
        m_lastFailureTimeNs.store(now, std::memory_order_release);

        auto currentState = static_cast<CircuitState>(m_state.load(std::memory_order_acquire));

        switch (currentState) {
            case CircuitState::Closed: {
                auto count = m_failureCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count >= m_config.failureThreshold) {
                    // 尝试转换到 Open
                    int expected = static_cast<int>(CircuitState::Closed);
                    m_state.compare_exchange_strong(expected,
                        static_cast<int>(CircuitState::Open),
                        std::memory_order_acq_rel);
                }
                break;
            }

            case CircuitState::HalfOpen: {
                // 直接转换到 Open
                releaseHalfOpenProbe();
                int expected = static_cast<int>(CircuitState::HalfOpen);
                if (m_state.compare_exchange_strong(expected,
                        static_cast<int>(CircuitState::Open),
                        std::memory_order_acq_rel)) {
                    m_failureCount.store(0, std::memory_order_relaxed);
                    m_successCount.store(0, std::memory_order_relaxed);
                    m_halfOpenInFlight.store(0, std::memory_order_relaxed);
                }
                break;
            }

            case CircuitState::Open:
                break;
        }
    }

    /**
     * @brief 在熔断保护下执行返回 expected-like 结果的函数
     * @tparam F 无参可调用对象类型，返回值需要满足 CircuitBreakerExpected，
     *           且 error_type 可从 CircuitBreakerError 构造
     * @param f 待执行的函数；熔断器允许请求时才会调用
     * @return 函数返回的结果；熔断打开时返回包含 CircuitBreakerError::Open 的失败结果
     * @details 不捕获异常，也不通过异常判断失败。返回结果 has_value() 为 true 时记录成功，
     *          否则记录失败并原样返回失败结果。error_type 需可从 CircuitBreakerError 构造。
     */
    template<typename F>
    requires detail::CircuitBreakerOperation<F&&>
    auto execute(F&& f) -> std::invoke_result_t<F&&> {
        using Result = std::remove_cvref_t<std::invoke_result_t<F&&>>;

        if (!allowRequest()) {
            return makeOpenResult<Result>();
        }

        auto result = std::invoke(std::forward<F>(f));
        if (result.has_value()) {
            onSuccess();
        } else {
            onFailure();
        }
        return result;
    }

    /**
     * @brief 在熔断保护下执行返回 expected-like 结果的函数，失败时调用降级函数
     * @tparam F 主函数类型，返回值需要满足 CircuitBreakerExpected
     * @tparam Fallback 降级函数类型，返回值需要与主函数完全一致
     * @param f 待执行的主函数；熔断器允许请求时才会调用
     * @param fallback 降级函数；主函数返回失败结果或熔断打开时调用
     * @return 主函数成功结果或降级函数结果
     * @details 不捕获异常，也不通过异常判断失败。主函数返回失败结果时记录失败，再返回
     *          fallback() 的结果；熔断打开时不调用主函数，直接返回 fallback()。
     */
    template<typename F, typename Fallback>
    requires detail::CircuitBreakerFallbackPair<F&&, Fallback&&>
    auto executeWithFallback(F&& f, Fallback&& fallback) -> std::invoke_result_t<F&&> {
        if (!allowRequest()) {
            return std::invoke(std::forward<Fallback>(fallback));
        }

        auto result = std::invoke(std::forward<F>(f));
        if (result.has_value()) {
            onSuccess();
            return result;
        }

        onFailure();
        return std::invoke(std::forward<Fallback>(fallback));
    }

    /**
     * @brief 获取当前熔断器状态
     * @return 当前状态枚举值
     */
    CircuitState state() const {
        return static_cast<CircuitState>(m_state.load(std::memory_order_acquire));
    }

    /**
     * @brief 获取当前状态的可读字符串
     * @return 状态字符串（"CLOSED"、"OPEN"、"HALF_OPEN"）
     */
    const char* stateString() const {
        switch (state()) {
            case CircuitState::Closed: return "CLOSED";
            case CircuitState::Open: return "OPEN";
            case CircuitState::HalfOpen: return "HALF_OPEN";
        }
        return "UNKNOWN";
    }

    /**
     * @brief 获取失败计数
     * @return 当前失败次数
     */
    size_t failureCount() const {
        return m_failureCount.load(std::memory_order_acquire);
    }

    /**
     * @brief 获取成功计数
     * @return 当前成功次数
     */
    size_t successCount() const {
        return m_successCount.load(std::memory_order_acquire);
    }

    /**
     * @brief 重置熔断器为关闭状态
     */
    void reset() {
        m_state.store(static_cast<int>(CircuitState::Closed), std::memory_order_release);
        m_failureCount.store(0, std::memory_order_relaxed);
        m_successCount.store(0, std::memory_order_relaxed);
        m_halfOpenInFlight.store(0, std::memory_order_relaxed);
        m_lastFailureTimeNs.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief 强制将熔断器设置为开启状态
     */
    void forceOpen() {
        m_lastFailureTimeNs.store(nowNs(), std::memory_order_release);
        m_failureCount.store(0, std::memory_order_relaxed);
        m_successCount.store(0, std::memory_order_relaxed);
        m_halfOpenInFlight.store(0, std::memory_order_relaxed);
        m_state.store(static_cast<int>(CircuitState::Open), std::memory_order_release);
    }

    /**
     * @brief 获取熔断器配置
     * @return 配置的常引用
     */
    const CircuitBreakerConfig& config() const { return m_config; }

private:
    template<detail::CircuitBreakerOpenExpected Result>
    static Result makeOpenResult() {
        using Error = typename std::remove_cvref_t<Result>::error_type;
        return std::unexpected<Error>{Error{CircuitBreakerError::Open}};
    }

    static CircuitBreakerConfig normalizeConfig(CircuitBreakerConfig config) {
        if (config.failureThreshold == 0) {
            config.failureThreshold = 1;
        }
        if (config.successThreshold == 0) {
            config.successThreshold = 1;
        }
        if (config.halfOpenMaxRequests == 0) {
            config.halfOpenMaxRequests = 1;
        }
        if (config.resetTimeout < std::chrono::seconds::zero()) {
            config.resetTimeout = std::chrono::seconds::zero();
        }
        return config;
    }

    static int64_t timeoutToNs(std::chrono::seconds timeout) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
    }

    static int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count();
    }

    bool tryAcquireHalfOpenProbe() {
        auto current = m_halfOpenInFlight.load(std::memory_order_relaxed);
        while (current < m_config.halfOpenMaxRequests) {
            if (m_halfOpenInFlight.compare_exchange_weak(current,
                    current + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void releaseHalfOpenProbe() {
        auto current = m_halfOpenInFlight.load(std::memory_order_relaxed);
        while (current > 0) {
            if (m_halfOpenInFlight.compare_exchange_weak(current,
                    current - 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return;
            }
        }
    }

    CircuitBreakerConfig m_config;
    int64_t m_resetTimeoutNs;
    std::atomic<int> m_state;
    std::atomic<size_t> m_failureCount;
    std::atomic<size_t> m_successCount;
    std::atomic<size_t> m_halfOpenInFlight;
    std::atomic<int64_t> m_lastFailureTimeNs;
};

using CircuitBreaker = BasicCircuitBreaker<>;

} // namespace galay::utils

#endif // GALAY_UTILS_CIRCUIT_BREAKER_HPP
