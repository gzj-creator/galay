/**
 * @file time.hpp
 * @brief 轻量时间工具
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供秒表、截止时间和退避策略等纯工具类型。不创建线程，
 *          不挂接调度器，不提供阻塞等待语义。
 */

#ifndef GALAY_UTILS_TIME_HPP
#define GALAY_UTILS_TIME_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

namespace galay::utils {

/**
 * @brief 通用时间工具
 * @details 提供系统时钟时间戳和本地/GMT 时间格式化。只做纯计算和标准库
 *          时间转换，不创建线程，不负责 sleep 或调度。
 */
class Time {
public:
    /**
     * @brief 获取 Unix epoch 至当前时间的毫秒数
     * @return 当前系统时间毫秒时间戳
     */
    static std::int64_t currentTimeMs() {
        return nowSinceEpoch<std::chrono::milliseconds>();
    }

    /**
     * @brief 获取 Unix epoch 至当前时间的微秒数
     * @return 当前系统时间微秒时间戳
     */
    static std::int64_t currentTimeUs() {
        return nowSinceEpoch<std::chrono::microseconds>();
    }

    /**
     * @brief 获取 Unix epoch 至当前时间的纳秒数
     * @return 当前系统时间纳秒时间戳
     */
    static std::int64_t currentTimeNs() {
        return nowSinceEpoch<std::chrono::nanoseconds>();
    }

    /**
     * @brief 格式化指定时间戳
     * @param timestamp Unix epoch 秒级时间戳
     * @param format strftime 格式字符串；为空时返回空字符串
     * @param utc 为 true 时按 UTC/GMT 格式化，否则按本地时区格式化
     * @return 格式化后的时间字符串；转换或格式化失败时返回空字符串
     */
    static std::string formatTime(std::time_t timestamp, const char* format, bool utc = false) {
        if (format == nullptr) {
            return {};
        }

        std::tm tmBuffer{};
        std::tm* tm = nullptr;

#if defined(_WIN32)
        const errno_t err = utc ? gmtime_s(&tmBuffer, &timestamp)
                                : localtime_s(&tmBuffer, &timestamp);
        if (err != 0) {
            return {};
        }
        tm = &tmBuffer;
#else
        tm = utc ? gmtime_r(&timestamp, &tmBuffer)
                 : localtime_r(&timestamp, &tmBuffer);
        if (tm == nullptr) {
            return {};
        }
#endif

        char buffer[256]{};
        if (std::strftime(buffer, sizeof(buffer), format, tm) == 0) {
            return {};
        }
        return std::string(buffer);
    }

    /**
     * @brief 获取当前 GMT 时间字符串
     * @param format strftime 格式字符串
     * @return 当前 GMT 时间字符串
     */
    static std::string currentGMTTime(const char* format = "%a, %d %b %Y %H:%M:%S GMT") {
        return formatTime(std::time(nullptr), format, true);
    }

    /**
     * @brief 获取当前本地时间字符串
     * @param format strftime 格式字符串
     * @return 当前本地时间字符串
     */
    static std::string currentLocalTime(const char* format = "%Y-%m-%d %H:%M:%S") {
        return formatTime(std::time(nullptr), format, false);
    }

private:
    template<typename Duration>
    static std::int64_t nowSinceEpoch() {
        const auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<Duration>(now.time_since_epoch()).count();
    }
};

/**
 * @brief 轻量秒表
 * @details 使用模板时钟记录起点并计算已经过时间。非线程安全。
 * @tparam Clock 满足 std::chrono 时钟接口的类型
 */
template<typename Clock = std::chrono::steady_clock>
class StopWatch {
public:
    using clock_type = Clock; ///< 时钟类型
    using duration = typename Clock::duration; ///< 时钟 duration 类型
    using time_point = typename Clock::time_point; ///< 时钟 time_point 类型

    /**
     * @brief 以当前时刻作为起点构造秒表
     */
    StopWatch()
        : m_start(Clock::now()) {}

    /**
     * @brief 以指定时刻作为起点构造秒表
     * @param start 起始时刻
     */
    explicit StopWatch(time_point start)
        : m_start(start) {}

    /**
     * @brief 重置起点为当前时刻
     */
    void reset() {
        m_start = Clock::now();
    }

    /**
     * @brief 获取经过时间
     * @return 当前时刻减去起点
     */
    duration elapsed() const {
        return Clock::now() - m_start;
    }

    /**
     * @brief 获取经过毫秒数
     * @return 经过时间的毫秒浮点值
     */
    double elapsedMs() const {
        return std::chrono::duration<double, std::milli>(elapsed()).count();
    }

    /**
     * @brief 获取当前起点
     * @return 起点时刻
     */
    time_point startTime() const {
        return m_start;
    }

private:
    time_point m_start;
};

/**
 * @brief 截止时间工具
 * @details 保存一个绝对截止时刻，提供过期判断和剩余时间查询。非线程安全。
 * @tparam Clock 满足 std::chrono 时钟接口的类型
 */
template<typename Clock = std::chrono::steady_clock>
class Deadline {
public:
    using clock_type = Clock; ///< 时钟类型
    using duration = typename Clock::duration; ///< 时钟 duration 类型
    using time_point = typename Clock::time_point; ///< 时钟 time_point 类型

    /**
     * @brief 以绝对时刻构造截止时间
     * @param deadline 截止时刻
     */
    explicit Deadline(time_point deadline)
        : m_deadline(deadline) {}

    /**
     * @brief 构造从当前时刻开始的一段相对截止时间
     * @tparam Duration std::chrono duration 类型
     * @param timeout 相对超时时长，小于等于 0 时立即过期
     * @return 截止时间对象
     */
    template<typename Duration>
    static Deadline fromNow(Duration timeout) {
        return Deadline(Clock::now() + std::chrono::duration_cast<duration>(timeout));
    }

    /**
     * @brief 判断是否已经到期
     * @return 当前时刻大于等于截止时刻时返回 true
     */
    bool expired() const {
        return Clock::now() >= m_deadline;
    }

    /**
     * @brief 获取剩余时间
     * @return 未过期时返回剩余 duration，已过期时返回 duration::zero()
     */
    duration remaining() const {
        const auto now = Clock::now();
        if (now >= m_deadline) {
            return duration::zero();
        }
        return m_deadline - now;
    }

    /**
     * @brief 获取绝对截止时刻
     * @return 截止时刻
     */
    time_point timePoint() const {
        return m_deadline;
    }

private:
    time_point m_deadline;
};

/**
 * @brief 退避策略
 * @details 提供固定、线性和指数退避序列。非线程安全，不负责 sleep 或调度。
 */
class Backoff {
public:
    using duration = std::chrono::nanoseconds; ///< 内部统一保存的时间单位

    /**
     * @brief 退避策略类型
     */
    enum class Strategy {
        Fixed, ///< 固定退避
        Linear, ///< 线性递增退避
        Exponential ///< 指数递增退避
    };

    /**
     * @brief 创建固定退避策略
     * @tparam Duration std::chrono duration 类型
     * @param value 每次返回的退避时长；负数会被截断为 0
     * @return 退避策略
     */
    template<typename Duration>
    static Backoff fixed(Duration value) {
        return Backoff(Strategy::Fixed, clampDuration(value), duration::zero(), 1.0, clampDuration(value));
    }

    /**
     * @brief 创建线性退避策略
     * @tparam Duration std::chrono duration 类型
     * @param initial 初始退避时长；负数会被截断为 0
     * @param step 每次递增时长；负数会被截断为 0
     * @param max 最大退避时长；负数会被截断为 0
     * @return 退避策略
     */
    template<typename Duration>
    static Backoff linear(Duration initial, Duration step, Duration max) {
        return Backoff(Strategy::Linear, clampDuration(initial), clampDuration(step), 1.0, clampDuration(max));
    }

    /**
     * @brief 创建指数退避策略
     * @tparam Duration std::chrono duration 类型
     * @param initial 初始退避时长；负数会被截断为 0
     * @param multiplier 指数倍数；小于 1 时按 1 处理
     * @param max 最大退避时长；负数会被截断为 0
     * @return 退避策略
     */
    template<typename Duration>
    static Backoff exponential(Duration initial, double multiplier, Duration max) {
        return Backoff(Strategy::Exponential, clampDuration(initial), duration::zero(),
                       std::max(1.0, multiplier), clampDuration(max));
    }

    /**
     * @brief 返回下一次退避时长并推进尝试次数
     * @return 下一次退避时长
     */
    duration next() {
        duration value = duration::zero();
        switch (m_strategy) {
        case Strategy::Fixed:
            value = m_initial;
            break;
        case Strategy::Linear:
            value = m_initial + m_step * m_attempts;
            break;
        case Strategy::Exponential:
            value = exponentialValue();
            break;
        }

        ++m_attempts;
        return std::min(value, m_max);
    }

    /**
     * @brief 重置尝试次数
     */
    void reset() {
        m_attempts = 0;
    }

    /**
     * @brief 获取已经生成的退避次数
     * @return 尝试次数
     */
    std::size_t attempts() const {
        return m_attempts;
    }

    /**
     * @brief 获取策略类型
     * @return 策略类型
     */
    Strategy strategy() const {
        return m_strategy;
    }

private:
    Backoff(Strategy strategy, duration initial, duration step, double multiplier, duration max)
        : m_strategy(strategy)
        , m_initial(initial)
        , m_step(step)
        , m_multiplier(multiplier)
        , m_max(max) {}

    template<typename Duration>
    static duration clampDuration(Duration value) {
        const auto converted = std::chrono::duration_cast<duration>(value);
        return converted < duration::zero() ? duration::zero() : converted;
    }

    duration exponentialValue() const {
        long double ticks = static_cast<long double>(m_initial.count());
        for (std::size_t i = 0; i < m_attempts; ++i) {
            ticks *= m_multiplier;
            if (ticks >= static_cast<long double>(m_max.count())) {
                return m_max;
            }
        }
        return duration(static_cast<duration::rep>(ticks));
    }

    Strategy m_strategy;
    duration m_initial;
    duration m_step;
    double m_multiplier;
    duration m_max;
    std::size_t m_attempts = 0;
};

} // namespace galay::utils

#endif // GALAY_UTILS_TIME_HPP
