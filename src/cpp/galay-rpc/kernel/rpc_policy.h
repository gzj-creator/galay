/**
 * @file rpc_policy.h
 * @brief RPC重试和治理策略
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 定义Phase 6托管客户端使用的重试、限流和熔断策略。控制器均为
 *          header-only值类型/轻量状态对象；判断接口不阻塞，协程重试通过sleep挂起。
 */

#ifndef GALAY_RPC_POLICY_H
#define GALAY_RPC_POLICY_H

#include "rpc_call.h"
#include "../protoc/rpc_error.h"
#include "../../galay-kernel/common/sleep.hpp"
#include "../../galay-utils/tool/circuit_breaker.hpp"
#include "../../galay-utils/tool/rate_limiter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

namespace galay::rpc
{

/**
 * @brief RPC重试策略
 *
 * @details max_attempts是总尝试次数而不是额外重试次数；默认只对幂等调用生效。
 *          retryable_errors为空时不重试任何错误。
 */
struct RpcRetryPolicy {
    uint32_t max_attempts = 1;  ///< 总尝试次数，0会按1处理
    bool require_idempotent = true;  ///< 是否要求RpcCallOptions::idempotent()
    std::vector<RpcErrorCode> retryable_errors;  ///< 可重试错误码白名单
    std::chrono::milliseconds initial_backoff{1};  ///< 首次重试前退避
    std::chrono::milliseconds max_backoff{50};  ///< 最大退避
    double jitter_ratio = 0.2;  ///< 抖动比例，0表示关闭
};

/**
 * @brief 限流策略
 */
struct RpcRateLimitPolicy {
    bool enabled = false;  ///< 是否启用限流
    size_t capacity = 1024;  ///< 最大令牌/许可数量
    double refill_per_second = 1024.0;  ///< 令牌填充速率；<=0时使用计数信号量
};

/**
 * @brief 熔断策略
 */
struct RpcCircuitBreakerPolicy {
    bool enabled = false;  ///< 是否启用熔断
    size_t failure_threshold = 5;  ///< 连续失败阈值
    size_t success_threshold = 1;  ///< 半开恢复成功阈值
    size_t half_open_max_requests = 1;  ///< 半开探测并发上限
    std::chrono::seconds reset_timeout{30};  ///< open到half-open等待时间
};

/**
 * @brief RPC治理策略
 */
struct RpcGovernancePolicy {
    RpcRateLimitPolicy rate_limit;  ///< 限流配置
    RpcCircuitBreakerPolicy circuit_breaker;  ///< 熔断配置
};

/**
 * @brief RPC重试控制器
 *
 * @details runAsync()不会阻塞OS线程；每次退避通过galay sleep awaitable挂起。
 *          Operation需返回Task<std::expected<T, RpcError>>或兼容的Task结果。
 */
class RpcRetryController {
public:
    /// @brief 判断错误码是否在策略白名单内
    static bool isRetryable(const RpcRetryPolicy& policy, RpcErrorCode code) {
        return std::ranges::find(policy.retryable_errors, code) != policy.retryable_errors.end();
    }

    /// @brief 计算最终总尝试次数
    static uint32_t maxAttempts(const RpcRetryPolicy& policy, const RpcCallOptions& options) {
        if (options.maxAttempts().has_value()) {
            return std::max<uint32_t>(1, *options.maxAttempts());
        }
        return std::max<uint32_t>(1, policy.max_attempts);
    }

    /**
     * @brief 同步执行重试策略，供无I/O边界测试和轻量适配使用
     * @tparam Result std::expected<T, RpcError>兼容结果类型
     * @param policy 重试策略
     * @param options 调用选项
     * @param operation 每次尝试执行的操作
     * @return 成功结果、最终失败错误或deadline错误
     */
    template<typename Result, typename Operation>
    static Result runSync(const RpcRetryPolicy& policy,
                          const RpcCallOptions& options,
                          Operation&& operation) {
        auto deadline = options.effectiveDeadline(RpcClock::now());
        if (deadline.has_value() && *deadline <= RpcClock::now()) {
            return std::unexpected(RpcError(RpcErrorCode::DEADLINE_EXCEEDED,
                                            "RPC retry deadline exceeded"));
        }

        const uint32_t attempts = maxAttempts(policy, options);
        const bool retry_allowed = !policy.require_idempotent || options.idempotent();
        Result last = std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE, "RPC retry not attempted"));

        for (uint32_t attempt = 1; attempt <= attempts; ++attempt) {
            last = std::invoke(operation);
            if (last.has_value()) {
                return last;
            }
            if (attempt == attempts || !retry_allowed || !isRetryable(policy, last.error().code())) {
                return last;
            }
            if (deadline.has_value() && *deadline <= RpcClock::now()) {
                return std::unexpected(RpcError(RpcErrorCode::DEADLINE_EXCEEDED,
                                                "RPC retry deadline exceeded"));
            }
        }
        return last;
    }

    /**
     * @brief 异步执行重试策略
     * @note 退避通过sleep挂起协程，不使用阻塞锁、条件变量或线程sleep。
     */
    template<typename Result, typename Operation>
    static kernel::Task<Result> runAsync(const RpcRetryPolicy& policy,
                                         const RpcCallOptions& options,
                                         Operation&& operation) {
        auto deadline = options.effectiveDeadline(RpcClock::now());
        if (deadline.has_value() && *deadline <= RpcClock::now()) {
            co_return Result(std::unexpected(RpcError(RpcErrorCode::DEADLINE_EXCEEDED,
                                                      "RPC retry deadline exceeded")));
        }

        const uint32_t attempts = maxAttempts(policy, options);
        const bool retry_allowed = !policy.require_idempotent || options.idempotent();
        Result last = std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE, "RPC retry not attempted"));

        for (uint32_t attempt = 1; attempt <= attempts; ++attempt) {
            auto task_result = co_await std::invoke(operation);
            last = unwrapTaskResult<Result>(std::move(task_result));
            if (last.has_value()) {
                co_return last;
            }
            if (attempt == attempts || !retry_allowed || !isRetryable(policy, last.error().code())) {
                co_return last;
            }

            const auto delay = nextBackoff(policy, attempt);
            if (deadline.has_value()) {
                const auto now = RpcClock::now();
                if (*deadline <= now) {
                    co_return Result(std::unexpected(RpcError(RpcErrorCode::DEADLINE_EXCEEDED,
                                                              "RPC retry deadline exceeded")));
                }
                if (delay > std::chrono::milliseconds::zero() && now + delay >= *deadline) {
                    co_return Result(std::unexpected(RpcError(RpcErrorCode::DEADLINE_EXCEEDED,
                                                              "RPC retry deadline exceeded")));
                }
            }
            if (delay > std::chrono::milliseconds::zero()) {
                co_await kernel::sleep(delay);
            }
        }
        co_return last;
    }

private:
    static std::chrono::milliseconds nextBackoff(const RpcRetryPolicy& policy, uint32_t attempt) {
        auto base = policy.initial_backoff;
        if (base <= std::chrono::milliseconds::zero()) {
            return std::chrono::milliseconds::zero();
        }
        for (uint32_t i = 1; i < attempt; ++i) {
            base *= 2;
            if (base >= policy.max_backoff) {
                base = policy.max_backoff;
                break;
            }
        }
        if (policy.max_backoff > std::chrono::milliseconds::zero()) {
            base = std::min(base, policy.max_backoff);
        }
        if (policy.jitter_ratio <= 0.0 || base.count() <= 0) {
            return base;
        }

        thread_local std::minstd_rand rng{std::random_device{}()};
        const auto spread = static_cast<int64_t>(static_cast<double>(base.count()) * policy.jitter_ratio);
        if (spread <= 0) {
            return base;
        }
        std::uniform_int_distribution<int64_t> dist(-spread, spread);
        return std::chrono::milliseconds(std::max<int64_t>(0, base.count() + dist(rng)));
    }

    template<typename Result, typename AwaitResult>
    static Result unwrapTaskResult(AwaitResult&& await_result) {
        if constexpr (requires { await_result.has_value(); await_result.error().message(); }) {
            if (!await_result.has_value()) {
                return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR,
                                                await_result.error().message()));
            }
            return std::move(await_result.value());
        } else {
            return std::forward<AwaitResult>(await_result);
        }
    }
};

/**
 * @brief 非阻塞治理控制器
 *
 * @details 组合同仓库galay-utils中的无锁限流器和熔断器。tryAcquire()只做快速
 *          原子判定；调用完成后由onSuccess/onFailure/release记录结果和归还许可。
 */
class RpcGovernanceController {
public:
    explicit RpcGovernanceController(RpcGovernancePolicy policy = {})
        : m_policy(policy)
        , m_semaphore(policy.rate_limit.capacity)
        , m_bucket(policy.rate_limit.refill_per_second, policy.rate_limit.capacity)
        , m_breaker(makeBreakerConfig(policy.circuit_breaker))
    {
    }

    /**
     * @brief 尝试通过限流和熔断前置检查
     * @return 成功或RATE_LIMITED/CIRCUIT_OPEN
     */
    std::expected<void, RpcError> tryAcquire() {
        if (m_policy.circuit_breaker.enabled &&
            m_breaker.state() == utils::CircuitState::Open &&
            !m_open_observed.exchange(true, std::memory_order_acq_rel)) {
            return std::unexpected(RpcError(RpcErrorCode::CIRCUIT_OPEN, "RPC circuit breaker is open"));
        }
        if (m_policy.circuit_breaker.enabled && !m_breaker.allowRequest()) {
            return std::unexpected(RpcError(RpcErrorCode::CIRCUIT_OPEN, "RPC circuit breaker is open"));
        }

        if (!m_policy.rate_limit.enabled) {
            m_acquired_rate_permits.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        const bool acquired = m_policy.rate_limit.refill_per_second > 0.0
            ? m_bucket.tryAcquire()
            : m_semaphore.tryAcquire();
        if (!acquired) {
            return std::unexpected(RpcError(RpcErrorCode::RATE_LIMITED, "RPC rate limited"));
        }
        m_acquired_rate_permits.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    /// @brief 记录成功调用
    void onSuccess() {
        if (m_policy.circuit_breaker.enabled) {
            m_breaker.onSuccess();
            if (m_breaker.state() == utils::CircuitState::Closed) {
                m_open_observed.store(false, std::memory_order_release);
            }
        }
    }

    /// @brief 记录失败调用
    void onFailure() {
        if (m_policy.circuit_breaker.enabled) {
            m_breaker.onFailure();
            if (m_breaker.state() == utils::CircuitState::Open) {
                m_open_observed.store(false, std::memory_order_release);
            }
        }
    }

    /// @brief 归还限流许可；令牌桶模式无需归还
    void release() {
        auto current = m_acquired_rate_permits.load(std::memory_order_relaxed);
        while (current > 0) {
            if (m_acquired_rate_permits.compare_exchange_weak(current,
                    current - 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                if (m_policy.rate_limit.enabled && m_policy.rate_limit.refill_per_second <= 0.0) {
                    m_semaphore.release();
                }
                return;
            }
        }
    }

    /// @brief 当前熔断状态
    utils::CircuitState circuitState() const { return m_breaker.state(); }

private:
    static utils::CircuitBreakerConfig makeBreakerConfig(const RpcCircuitBreakerPolicy& policy) {
        utils::CircuitBreakerConfig config;
        config.failureThreshold = policy.failure_threshold;
        config.successThreshold = policy.success_threshold;
        config.halfOpenMaxRequests = policy.half_open_max_requests;
        config.resetTimeout = policy.reset_timeout;
        return config;
    }

    RpcGovernancePolicy m_policy;
    utils::CountingSemaphore m_semaphore;
    utils::TokenBucketLimiter m_bucket;
    utils::CircuitBreaker m_breaker;
    std::atomic<size_t> m_acquired_rate_permits{0};
    std::atomic<bool> m_open_observed{false};
};

} // namespace galay::rpc

#endif // GALAY_RPC_POLICY_H
