/**
 * @file rate_limiter.hpp
 * @brief 限流器集合
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供四种限流器实现：计数信号量、令牌桶、滑动窗口和漏桶。
 *          所有限流器均为无锁非阻塞 API，失败时 tryAcquire() 返回 false。
 */

#ifndef GALAY_UTILS_RATE_LIMITER_HPP
#define GALAY_UTILS_RATE_LIMITER_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

namespace galay::utils {

namespace detail {

inline constexpr int64_t kRateLimiterPrecision = 1000000;

static_assert(std::atomic<size_t>::is_always_lock_free,
    "Rate limiter requires lock-free size_t atomics.");
static_assert(std::atomic<int64_t>::is_always_lock_free,
    "Rate limiter requires lock-free int64_t atomics.");

inline int64_t toRateLimiterUnits(size_t value) noexcept {
    constexpr size_t maxWholeUnits =
        static_cast<size_t>(std::numeric_limits<int64_t>::max() / kRateLimiterPrecision);
    if (value > maxWholeUnits) {
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(value) * kRateLimiterPrecision;
}

inline int64_t toRateLimiterUnits(double value) noexcept {
    if (!(value > 0.0)) {
        return 0;
    }

    const double scaled = value * static_cast<double>(kRateLimiterPrecision);
    if (scaled >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(scaled);
}

inline size_t fromRateLimiterUnits(int64_t units) noexcept {
    if (units <= 0) {
        return 0;
    }
    return static_cast<size_t>(units / kRateLimiterPrecision);
}

inline int64_t saturatingAddUnits(int64_t lhs, int64_t rhs) noexcept {
    if (rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) {
        return std::numeric_limits<int64_t>::max();
    }
    return lhs + rhs;
}

inline int64_t nonNegativeSteadyTicks(std::chrono::milliseconds duration) noexcept {
    if (duration <= std::chrono::milliseconds::zero()) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration).count();
}

} // namespace detail

/**
 * @note 本模块不提供协程 awaitable。限流器仅暴露同步非阻塞的 tryAcquire() API，
 *       可在协程调度线程上快速判定；需要挂起、唤醒、重试或超时时由上层运行时适配。
 */

/**
 * @brief 无锁计数信号量（非阻塞获取）
 * @details 使用原子计数器实现许可的获取和释放，不阻塞调用线程。
 */
class CountingSemaphore {
public:
    explicit CountingSemaphore(size_t initial = 0)
        : m_count(initial) {}

    /**
     * @brief 非阻塞尝试获取许可
     * @param count 请求的许可数量
     * @return 成功获取返回 true，否则返回 false
     */
    bool tryAcquire(size_t count = 1) {
        size_t current = m_count.load(std::memory_order_acquire);
        while (current >= count) {
            if (m_count.compare_exchange_weak(current, current - count,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 释放许可
     * @param count 释放的许可数量
     */
    void release(size_t count = 1) {
        m_count.fetch_add(count, std::memory_order_release);
    }

    /**
     * @brief 获取当前可用许可数量
     * @return 当前许可数量
     */
    size_t available() const {
        return m_count.load(std::memory_order_acquire);
    }

private:
    std::atomic<size_t> m_count;
};

/**
 * @brief 非阻塞令牌桶限流器
 * @details 按令牌/秒速率填充令牌，直到容量上限。线程安全，不阻塞调用者。
 */
class TokenBucketLimiter {
public:
    TokenBucketLimiter(double rate, size_t capacity)
        : m_rate_units(detail::toRateLimiterUnits(rate))
        , m_capacity_units(detail::toRateLimiterUnits(capacity))
        , m_tokens(detail::toRateLimiterUnits(capacity))
        , m_last_refill_time(nowTicks()) {}

    /**
     * @brief 非阻塞尝试消费令牌
     * @param tokens 请求的令牌数量
     * @return 令牌充足返回 true，否则返回 false
     */
    bool tryAcquire(size_t tokens = 1) {
        refill();

        int64_t needed = detail::toRateLimiterUnits(tokens);
        if (needed == 0) {
            return true;
        }

        int64_t current = m_tokens.load(std::memory_order_acquire);

        while (current >= needed) {
            if (m_tokens.compare_exchange_weak(current, current - needed,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 获取当前可用令牌数
     * @return 令牌数量（浮点值）
     */
    double availableTokens() const {
        return static_cast<double>(m_tokens.load(std::memory_order_acquire)) /
            detail::kRateLimiterPrecision;
    }

    /**
     * @brief 设置令牌填充速率
     * @param rate 每秒填充的令牌数
     */
    void setRate(double rate) {
        refill();
        m_rate_units.store(detail::toRateLimiterUnits(rate), std::memory_order_release);
        m_last_refill_time.store(nowTicks(), std::memory_order_release);
    }

    /**
     * @brief 设置桶容量并裁剪当前令牌数
     * @param capacity 最大令牌数量
     */
    void setCapacity(size_t capacity) {
        int64_t max_tokens = detail::toRateLimiterUnits(capacity);
        m_capacity_units.store(max_tokens, std::memory_order_release);
        int64_t current = m_tokens.load(std::memory_order_acquire);
        while (current > max_tokens) {
            if (m_tokens.compare_exchange_weak(current, max_tokens,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                break;
            }
        }
    }

    /**
     * @brief 获取配置的填充速率
     * @return 每秒令牌数
     */
    double rate() const {
        return static_cast<double>(m_rate_units.load(std::memory_order_acquire)) /
            detail::kRateLimiterPrecision;
    }

    /**
     * @brief 获取配置的桶容量
     * @return 最大令牌数量
     */
    size_t capacity() const {
        return detail::fromRateLimiterUnits(m_capacity_units.load(std::memory_order_acquire));
    }

private:
    static int64_t nowTicks() {
        return static_cast<int64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    void refill() {
        int64_t now = nowTicks();
        int64_t last_time = m_last_refill_time.load(std::memory_order_acquire);

        while (now > last_time) {
            int64_t rate_units = m_rate_units.load(std::memory_order_acquire);
            if (rate_units <= 0) {
                if (m_last_refill_time.compare_exchange_weak(last_time, now,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return;
                }
                continue;
            }

            double elapsed_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::duration(now - last_time)).count();
            double units_to_add = elapsed_seconds * static_cast<double>(rate_units);
            int64_t tokens_to_add =
                units_to_add >= static_cast<double>(std::numeric_limits<int64_t>::max())
                ? std::numeric_limits<int64_t>::max()
                : static_cast<int64_t>(units_to_add);
            if (tokens_to_add <= 0) {
                return;
            }

            if (m_last_refill_time.compare_exchange_weak(last_time, now,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                int64_t max_tokens = m_capacity_units.load(std::memory_order_acquire);
                int64_t current = m_tokens.load(std::memory_order_acquire);
                int64_t desired;
                do {
                    desired = std::min(max_tokens,
                        detail::saturatingAddUnits(current, tokens_to_add));
                } while (!m_tokens.compare_exchange_weak(current, desired,
                    std::memory_order_acq_rel, std::memory_order_acquire));
                return;
            }
        }
    }

    std::atomic<int64_t> m_rate_units;
    std::atomic<int64_t> m_capacity_units;
    std::atomic<int64_t> m_tokens;
    std::atomic<int64_t> m_last_refill_time;
};

/**
 * @brief 无锁滑动窗口限流器
 * @details 每次成功获取记录时间戳，超过窗口内最大请求数时返回 false。
 *          使用固定槽位和原子 CAS 记录请求，不阻塞调用线程。
 */
class SlidingWindowLimiter {
public:
    SlidingWindowLimiter(size_t maxRequests, std::chrono::milliseconds windowSize)
        : m_max_requests(maxRequests)
        , m_window_size(windowSize)
        , m_window_ticks(detail::nonNegativeSteadyTicks(windowSize))
        , m_requests(maxRequests) {
        for (auto& request : m_requests) {
            request.store(std::numeric_limits<int64_t>::min(), std::memory_order_relaxed);
        }
    }

    /**
     * @brief 非阻塞尝试记录一次请求
     * @return 允许请求返回 true，被限流返回 false
     */
    bool tryAcquire() {
        if (m_max_requests == 0) {
            return false;
        }

        const int64_t now = nowTicks();
        const int64_t cutoff = now - m_window_ticks;
        const size_t start = m_probe_cursor.fetch_add(1, std::memory_order_relaxed);

        for (size_t offset = 0; offset < m_max_requests; ++offset) {
            const size_t index = (start + offset) % m_max_requests;
            int64_t observed = m_requests[index].load(std::memory_order_acquire);
            while (observed < cutoff) {
                if (m_requests[index].compare_exchange_weak(observed, now,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return true;
                }
            }
        }
        return false;
    }

    /**
     * @brief 获取配置的请求限制
     * @return 窗口内最大请求数
     */
    size_t maxRequests() const { return m_max_requests; }

    /**
     * @brief 获取配置的窗口大小
     * @return 滑动窗口时长
     */
    std::chrono::milliseconds windowSize() const { return m_window_size; }

private:
    static int64_t nowTicks() {
        return static_cast<int64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    size_t m_max_requests;
    std::chrono::milliseconds m_window_size;
    int64_t m_window_ticks;
    std::vector<std::atomic<int64_t>> m_requests;
    std::atomic<size_t> m_probe_cursor{0};
};

/**
 * @brief 非阻塞漏桶限流器
 * @details 桶按配置速率漏水，仅在不超过容量时接受新水。线程安全，不阻塞。
 */
class LeakyBucketLimiter {
public:
    LeakyBucketLimiter(double rate, size_t capacity)
        : m_rate_units(detail::toRateLimiterUnits(rate))
        , m_capacity_units(detail::toRateLimiterUnits(capacity))
        , m_water(0)
        , m_last_leak_time(nowTicks()) {}

    /**
     * @brief 非阻塞尝试添加水量
     * @param amount 请求的水量
     * @return 容量允许返回 true，否则返回 false
     */
    bool tryAcquire(size_t amount = 1) {
        leak();

        int64_t requested = detail::toRateLimiterUnits(amount);
        if (requested == 0) {
            return true;
        }

        int64_t max_water = m_capacity_units.load(std::memory_order_acquire);
        int64_t current = m_water.load(std::memory_order_acquire);

        while (current <= max_water && requested <= max_water - current) {
            if (m_water.compare_exchange_weak(current, current + requested,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 获取当前桶内水量
     * @return 水量（浮点值）
     */
    double currentWater() const {
        return static_cast<double>(m_water.load(std::memory_order_acquire)) /
            detail::kRateLimiterPrecision;
    }

    /**
     * @brief 获取配置的漏水速率
     * @return 每秒漏水单位数
     */
    double rate() const {
        return static_cast<double>(m_rate_units.load(std::memory_order_acquire)) /
            detail::kRateLimiterPrecision;
    }

    /**
     * @brief 获取配置的桶容量
     * @return 最大水量
     */
    size_t capacity() const {
        return detail::fromRateLimiterUnits(m_capacity_units.load(std::memory_order_acquire));
    }

private:
    static int64_t nowTicks() {
        return static_cast<int64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    void leak() {
        int64_t now = nowTicks();
        int64_t last_time = m_last_leak_time.load(std::memory_order_acquire);

        while (now > last_time) {
            int64_t rate_units = m_rate_units.load(std::memory_order_acquire);
            if (rate_units <= 0) {
                if (m_last_leak_time.compare_exchange_weak(last_time, now,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return;
                }
                continue;
            }

            double elapsed_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::duration(now - last_time)).count();
            double units_to_leak = elapsed_seconds * static_cast<double>(rate_units);
            int64_t leaked =
                units_to_leak >= static_cast<double>(std::numeric_limits<int64_t>::max())
                ? std::numeric_limits<int64_t>::max()
                : static_cast<int64_t>(units_to_leak);
            if (leaked <= 0) {
                return;
            }

            if (m_last_leak_time.compare_exchange_weak(last_time, now,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                int64_t current = m_water.load(std::memory_order_acquire);
                int64_t desired;
                do {
                    desired = std::max<int64_t>(0, current - leaked);
                } while (!m_water.compare_exchange_weak(current, desired,
                    std::memory_order_acq_rel, std::memory_order_acquire));
                return;
            }
        }
    }

    std::atomic<int64_t> m_rate_units;
    std::atomic<int64_t> m_capacity_units;
    std::atomic<int64_t> m_water;
    std::atomic<int64_t> m_last_leak_time;
};

} // namespace galay::utils

#endif // GALAY_UTILS_RATE_LIMITER_HPP
