/**
 * @file timeout.hpp
 * @brief 异步 IO awaitable 的超时支持
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供：
 * - TimeoutTimer：定时器子类，在超时时唤醒关联协程
 * - TimeoutSupport<Derived>：CRTP 混入，为任意 awaitable 添加 .timeout(ms) 方法
 * - WithTimeout<Awaitable>：组合 awaitable 与定时器的包装器，
 *   当内部操作未在规定时间内完成时注入超时错误
 */

#ifndef GALAY_KERNEL_TIMEOUT_HPP
#define GALAY_KERNEL_TIMEOUT_HPP

#include "kernel/common/error.h"
#include "kernel/common/timer.hpp"
#include "kernel/common/concepts.h"
#include "waker.h"
#include "scheduler.hpp"
#include <memory>
#include <expected>

namespace galay::kernel 
{

class TimeoutTimer final: public Timer
{
public:
    using ptr = std::shared_ptr<TimeoutTimer>;

    template<concepts::ChronoDuration Duration>
    TimeoutTimer(Duration duration)
        : Timer(duration) {}

    void setWaker(Waker waker) { m_waker = waker; }

    void handleTimeout() override {
        if(!cancelled() && !done()) {
            m_flag.fetch_or(static_cast<int>(TimerFlag::kTimeout), std::memory_order_release);
            m_waker.wakeUp();
        }
        Timer::handleTimeout();
    }

    bool timeouted() const {
        return (m_flag.load(std::memory_order_acquire) &
                static_cast<int>(TimerFlag::kTimeout)) != 0;
    }

private:
    Waker m_waker;
};

template<typename Awaitable>
struct WithTimeout;

/**
* @brief CRTP 基类，为 Awaitable 提供 timeout() 方法
*/
template<typename Derived>
struct TimeoutSupport {
    template<typename D = Derived>
    requires concepts::Awaitable<D>
    auto timeout(std::chrono::milliseconds t) && {
        return WithTimeout<Derived>{std::move(static_cast<Derived&>(*this)), t};
    }

    template<typename D = Derived>
    requires concepts::Awaitable<D>
    auto timeout(std::chrono::milliseconds t) & {
        return WithTimeout<Derived>{static_cast<Derived&>(*this), t};
    }
};

/**
 * @brief 超时包装器
 *
 * @details 对于 io_uring，使用独立的 timeout 操作；对于 epoll/kqueue，使用 timerfd。
 * 定时器状态存储在 IOController 中，生命周期与 TcpSocket 绑定。
 */
template<typename Awaitable>
struct WithTimeout {
    Awaitable m_inner;
    TimeoutTimer::ptr m_timer;

    WithTimeout(Awaitable&& inner, std::chrono::milliseconds timeout)
        : m_inner(std::move(inner)), m_timer(std::make_shared<TimeoutTimer>(timeout)) {}

    WithTimeout(Awaitable& inner, std::chrono::milliseconds timeout)
        : m_inner(std::move(inner)), m_timer(std::make_shared<TimeoutTimer>(timeout)) {}

    bool await_ready() { return m_inner.await_ready(); }

    template<typename Promise>
    requires concepts::AwaitableWith<Awaitable, Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        bool suspended = m_inner.await_suspend(handle);
        if (!suspended) {
            return false;
        }
        auto waker = Waker(handle);
        m_timer->setWaker(Waker(handle));
        waker.getScheduler()->addTimer(m_timer);
        return true;
    }

    auto await_resume() -> decltype(m_inner.await_resume()) {
        // 检查是否超时
        if (m_timer->timeouted()) [[unlikely]] {
            if constexpr (requires(Awaitable& awaitable) {
                awaitable.markTimeout();
            }) {
                m_inner.markTimeout();
            } else if constexpr (requires { m_inner.m_result; }) {
                // 历史 awaitable 通过写入 m_result 注入超时错误
                m_inner.m_result = std::unexpected(IOError(kTimeout, 0));
            }
        } else {
            m_timer->cancel();
        }
        return m_inner.await_resume();
    }

};

}



#endif
