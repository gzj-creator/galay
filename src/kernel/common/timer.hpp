/**
 * @file timer.hpp
 * @brief 定时器基类和回调驱动定时器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义 galay-kernel 中所有定时器管理器使用的核心 Timer 抽象。
 * 每个 Timer 存储一个相对延迟（纳秒），并在首次查询时延迟计算绝对过期时间。
 * 支持通过原子标志取消。CBTimer 通过 std::function 回调扩展 Timer。
 */

#ifndef GALAY_TIMER_HPP
#define GALAY_TIMER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "kernel/common/concepts.h"

namespace galay::kernel
{

/**
 * @brief 跟踪定时器生命周期状态的位掩码标志
 */
enum class TimerFlag : int {
    kDone = 1 << 0,    ///< 定时器已触发并完成
    kCancel = 1 << 1,  ///< 定时器被显式取消
    kTimeout = 1 << 2, ///< 保留用于未来的超时跟踪
};

/**
 * @brief galay-kernel 中所有定时器的基类
 *
 * @details 存储相对延迟，并在首次调用 getExpireTime() 时延迟计算
 * 绝对过期时间（steady-clock 纳秒）。支持通过原子标志取消；
 * 派生类重写 handleTimeout() 以实现自定义过期行为。
 */
class Timer
{
public:
    using ptr = std::shared_ptr<Timer>;

    /**
     * @brief 以指定延迟构造定时器
     * @tparam Duration 满足 ChronoDuration 的类型（如 std::chrono::milliseconds）
     * @param duration 到过期的时间
     *
     * @note 绝对过期时间在首次 getExpireTime() 调用时延迟计算，
     *       因此有效延迟从定时器首次被查询时开始，而非构造时。
     */
    template<concepts::ChronoDuration Duration>
    Timer(Duration duration) {
        // 存储相对时间间隔（纳秒）
        m_delay = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        // 过期时间延迟到 getExpireTime() 时计算
        m_expireTime = 0;
    }

    /**
     * @brief 定时器到期时调用
     *
     * @details 通过 kDone 标志标记定时器为已完成。派生类
     * 应在执行自身工作后调用此基类实现。
     */
    virtual void handleTimeout() {
        m_flag.fetch_or(static_cast<int>(TimerFlag::kDone), std::memory_order_release);
    }

    /**
     * @brief 检查定时器是否已完成
     * @return 若定时器已触发并完成则返回 true
     */
    bool done() const {
        return (m_flag.load(std::memory_order_acquire) &
                static_cast<int>(TimerFlag::kDone)) != 0;
    }

    /**
     * @brief 取消定时器
     *
     * @details 原子地设置 kCancel 标志。定时器管理器在
     * 调用 handleTimeout() 前检查此标志。
     */
    void cancel() {
        m_flag.fetch_or(static_cast<int>(TimerFlag::kCancel), std::memory_order_release);
    }

    /**
     * @brief 检查定时器是否已被取消
     * @return 若在过期前调用了 cancel() 则返回 true
     */
    bool cancelled() const {
        return (m_flag.load(std::memory_order_acquire) &
                static_cast<int>(TimerFlag::kCancel)) != 0;
    }

    /**
     * @brief 获取相对延迟（纳秒）
     * @return 构造时提供的延迟值
     */
    uint64_t getDelay() const { return m_delay; }

    /**
     * @brief 获取绝对过期时间（纳秒，steady-clock 纪元）
     *
     * @details 首次调用时计算 now + delay 并缓存结果。
     * 后续调用返回缓存值。
     *
     * @return 绝对过期时间（纳秒）
     */
    uint64_t getExpireTime() const {
        if (m_expireTime == 0) {
            // 第一次调用时计算过期时间
            auto now = std::chrono::steady_clock::now();
            auto expire_time = now + std::chrono::nanoseconds(m_delay);
            m_expireTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
                expire_time.time_since_epoch()).count();
        }
        return m_expireTime;
    }

protected:
    std::atomic<int> m_flag{0};
    uint64_t m_delay = 0;                  ///< 相对延迟（纳秒）
    mutable uint64_t m_expireTime = 0;     ///< 绝对过期时间（纳秒，延迟计算）
};

/**
 * @brief 回调驱动定时器
 *
 * @details 具体的 Timer，在过期时调用 std::function<void()>。
 * 若定时器已被取消或已完成则跳过调用。
 */
class CBTimer final: public Timer
{
public:
    /**
     * @brief 构造回调定时器
     * @tparam Duration ChronoDuration 类型
     * @param duration 到过期的时间
     * @param callback 超时时调用的函数
     */
    template<concepts::ChronoDuration Duration>
    CBTimer(Duration duration, std::function<void()>&& callback)
        : Timer(duration), m_callback(std::move(callback)) {}

    /**
     * @brief 若未被取消/已完成则调用存储的回调，然后标记完成
     */
    void handleTimeout() override {
        if(!cancelled() && !done()) {
            m_callback();
        }
        Timer::handleTimeout();
    }
private:
    std::function<void()> m_callback;
};

}

#endif
