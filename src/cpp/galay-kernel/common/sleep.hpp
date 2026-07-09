/**
 * @file sleep.hpp
 * @brief 基于协程的异步休眠
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供 sleep() 函数，返回一个在给定时长内挂起调用协程的 Awaitable。
 * 内部创建 SleepTimer 并注册到全局 TimerScheduler；超时时 Waker
 * 恢复挂起的协程。
 *
 * @code
 * co_await sleep(std::chrono::milliseconds(100));
 * @endcode
 */

#ifndef GALAY_KERNEL_SLEEP_HPP
#define GALAY_KERNEL_SLEEP_HPP

#include "timer.hpp"
#include <memory>
#include "concepts.h"
#include "../core/waker.h"
#include "../core/timer_scheduler.h"

namespace galay::kernel
{

/**
 * @brief 在到期时唤醒挂起协程的定时器
 *
 * @details 通过 Waker 扩展 Timer。超时时 Waker 恢复与
 * Waker 中存储的 coroutine_handle 关联的协程。
 */
class SleepTimer final: public Timer
{
public:
    using ptr = std::shared_ptr<SleepTimer>;

    /**
     * @brief 以指定延迟构造休眠定时器
     * @tparam Duration ChronoDuration 类型
     * @param duration 休眠时长
     */
    template<concepts::ChronoDuration Duration>
    SleepTimer(Duration duration)
        :Timer(duration) {}

private:
    SleepTimer(const SleepTimer&) = delete;
    SleepTimer& operator=(const SleepTimer&) = delete;
public:

    /**
     * @brief 存储超时时调用的 waker
     * @param waker 包装 coroutine_handle 的 waker
     */
    void setWaker(Waker waker) { m_waker = waker; }

    /**
     * @brief 恢复挂起的协程并标记此定时器完成
     */
    void handleTimeout() override {  m_waker.wakeUp(); Timer::handleTimeout(); }

private:
    Waker m_waker;
};

/**
 * @brief sleep() 返回的 Awaitable；将调用者挂起一段时长
 *
 * @details 在 await_suspend 时，将 SleepTimer 注册到全局
 * TimerScheduler。定时器触发时协程被恢复。
 */
struct SleepAwaitable
{
    SleepTimer::ptr m_timer;

    /**
     * @brief 以时长构造
     * @tparam Duration ChronoDuration 类型
     * @param duration 休眠时长
     */
    template<concepts::ChronoDuration Duration>
    SleepAwaitable(Duration duration)
        :m_timer(std::make_shared<SleepTimer>(duration)) {}

    SleepAwaitable(SleepAwaitable&&) noexcept = default;
    SleepAwaitable& operator=(SleepAwaitable&&) noexcept = default;

    /**
     * @brief 始终返回 false，使协程挂起
     */
    bool await_ready() { return false; }

    /**
     * @brief 将定时器注册到全局 TimerScheduler
     * @tparam Promise 协程 promise 类型
     * @param handle 挂起的协程句柄
     * @return 若协程应挂起则返回 true，注册失败则返回 false
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        m_timer->setWaker(Waker(handle));
        // 使用全局 TimerScheduler 而不是 IOScheduler 的定时器
        if(!TimerScheduler::getInstance()->addTimer(m_timer)) {
            return false;
        }
        return true;
    }

    /**
     * @brief 空操作；协程以 void 恢复
     */
    void await_resume() {}

private:
    SleepAwaitable(const SleepAwaitable&) = delete;
    SleepAwaitable& operator=(const SleepAwaitable&) = delete;
};

/**
 * @brief 创建休眠指定时长的 awaitable
 * @tparam Duration ChronoDuration 类型（如 std::chrono::milliseconds）
 * @param duration 休眠时长
 * @return 可被 co_await 的 SleepAwaitable
 *
 * @code
 * co_await sleep(std::chrono::seconds(5));
 * @endcode
 */
template<concepts::ChronoDuration Duration>
SleepAwaitable sleep(Duration duration) {
    return SleepAwaitable(duration);
}

}
#endif
