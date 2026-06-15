/**
 * @file async_waiter.h
 * @brief 异步等待器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供跨线程的协程等待机制，支持返回结果。
 * 典型用于 ComputeScheduler 计算任务完成后通知 IO 协程。
 *
 * 使用方式：
 * @code
 * // IO 任务中
 * AsyncWaiter<int> waiter;
 * scheduleTask(*computeScheduler, computeTask(&waiter));
 * auto result = co_await waiter.wait();  // 挂起等待
 * if (result) {
 *     // 使用 result.value()
 * }
 *
 * // 使用超时
 * auto result = co_await waiter.wait().timeout(100ms);
 * if (!result) {
 *     // 超时或错误
 * }
 *
 * // 计算任务中
 * Task<void> computeTask(AsyncWaiter<int>* waiter) {
 *     int result = heavyCompute();
 *     waiter->notify(result);  // 唤醒等待任务
 *     co_return;
 * }
 * @endcode
 */

#ifndef GALAY_KERNEL_ASYNC_WAITER_H
#define GALAY_KERNEL_ASYNC_WAITER_H

#include "galay-kernel/core/task.h"
#include "galay-kernel/core/timeout.hpp"
#include "galay-kernel/core/waker.h"
#include "galay-kernel/common/error.h"
#include <atomic>
#include <concepts>
#include <optional>
#include <expected>
#include <coroutine>
#include <type_traits>

namespace galay::kernel
{

template<typename T>
class AsyncWaiter;

/**
 * @brief AsyncWaiter 的等待体
 * @tparam T 等待结果类型
 * @details 支持和 `TimeoutSupport` 组合使用；超时后 `await_resume()` 返回 IOError。
 */
template<typename T>
class AsyncWaiterAwaitable : public TimeoutSupport<AsyncWaiterAwaitable<T>>
{
public:
    static_assert(std::movable<T> && (!std::is_void_v<T>),
                  "AsyncWaiterAwaitable<T> requires movable, non-void T");

    /**
     * @brief 构造等待体
     * @param waiter 关联的等待器；调用方需保证其在等待完成前保持有效
     */
    explicit AsyncWaiterAwaitable(AsyncWaiter<T>* waiter) : m_waiter(waiter) {}

    bool await_ready() const noexcept;  ///< 如果结果已经 ready，则返回 true 以避免挂起
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;  ///< 注册等待协程并在结果未就绪时挂起
    std::expected<T, IOError> await_resume() noexcept;  ///< 返回结果；若超时则返回 IOError(kTimeout, 0)

private:
    friend struct WithTimeout<AsyncWaiterAwaitable<T>>;
    AsyncWaiter<T>* m_waiter;
    std::expected<T, IOError> m_result;
};

/**
 * @brief void 特化的等待体
 * @details 用于不携带返回值、只传递完成信号的跨线程等待场景。
 */
template<>
class AsyncWaiterAwaitable<void> : public TimeoutSupport<AsyncWaiterAwaitable<void>>
{
public:
    /**
     * @brief 构造等待体
     * @param waiter 关联的等待器；调用方需保证其在等待完成前保持有效
     */
    explicit AsyncWaiterAwaitable(AsyncWaiter<void>* waiter) : m_waiter(waiter) {}

    bool await_ready() const noexcept;  ///< 如果完成信号已经到达，则返回 true 以避免挂起
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;  ///< 注册等待协程并在尚未完成时挂起
    std::expected<void, IOError> await_resume() noexcept { return m_result; }  ///< 返回完成结果；超时时返回 IOError(kTimeout, 0)

private:
    friend struct WithTimeout<AsyncWaiterAwaitable<void>>;
    AsyncWaiter<void>* m_waiter;
    std::expected<void, IOError> m_result;
};

/**
 * @brief 异步等待器
 *
 * @tparam T 结果类型
 *
 * @details 用于跨线程 Task 同步。一个任务调用 wait() 挂起，
 * 另一个线程/任务调用 notify() 设置结果并唤醒。
 *
 * @note 线程安全，每个 AsyncWaiter 实例只能使用一次
 */
template<typename T>
class AsyncWaiter
{
public:
    static_assert(std::movable<T> && (!std::is_void_v<T>),
                  "AsyncWaiter<T> requires movable, non-void T");
    AsyncWaiter() = default;

    // 禁止拷贝和移动
    AsyncWaiter(const AsyncWaiter&) = delete;
    AsyncWaiter& operator=(const AsyncWaiter&) = delete;
    AsyncWaiter(AsyncWaiter&&) = delete;
    AsyncWaiter& operator=(AsyncWaiter&&) = delete;

    /**
     * @brief 等待结果
     * @return 与当前等待器绑定的 awaitable，可继续叠加 `.timeout(...)`
     * @note 每个 AsyncWaiter 设计为单次使用；重复等待同一个实例不受支持
     */
    AsyncWaiterAwaitable<T> wait() {
        return AsyncWaiterAwaitable<T>(this);
    }

    /**
     * @brief 设置结果并唤醒等待的协程
     * @param result 结果值
     * @return true 成功通知，false 已经通知过
     * @note 允许由其他线程调用；成功后会把等待协程唤醒回其原调度器
     */
    bool notify(T result) {
        // 防止重复通知
        bool expected = false;
        if (!m_ready.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return false;  // 已经通知过
        }

        m_result = std::move(result);

        // 唤醒等待的协程（如果有）
        expected = true;
        if (m_waiting.compare_exchange_strong(expected, false,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            m_waker.wakeUp();
        }
        return true;
    }

    /**
     * @brief 检查是否正在等待
     * @return true 当前已有协程注册在该等待器上并处于挂起状态
     */
    bool isWaiting() const {
        return m_waiting.load(std::memory_order_acquire);
    }

    /**
     * @brief 检查结果是否就绪
     * @return true 已调用 notify() 且结果对等待侧可见
     */
    bool isReady() const {
        return m_ready.load(std::memory_order_acquire);
    }

private:
    friend class AsyncWaiterAwaitable<T>;

    std::optional<T> m_result;                  ///< 结果
    std::atomic<bool> m_waiting{false};         ///< 是否有协程在等待
    std::atomic<bool> m_ready{false};           ///< 结果是否就绪
    Waker m_waker;
};

/**
 * @brief void 特化版本
 * @details 用于只需要完成通知、不需要返回值的场景。
 */
template<>
class AsyncWaiter<void>
{
public:
    AsyncWaiter() = default;

    AsyncWaiter(const AsyncWaiter&) = delete;
    AsyncWaiter& operator=(const AsyncWaiter&) = delete;
    AsyncWaiter(AsyncWaiter&&) = delete;
    AsyncWaiter& operator=(AsyncWaiter&&) = delete;

    /**
     * @brief 等待完成通知
     * @return 与当前等待器绑定的 awaitable，可继续叠加 `.timeout(...)`
     */
    AsyncWaiterAwaitable<void> wait() {
        return AsyncWaiterAwaitable<void>(this);
    }

    /**
     * @brief 发送完成通知并唤醒等待协程
     * @return true 首次通知成功；false 已经通知过
     * @note 允许由其他线程调用；成功后会把等待协程唤醒回其原调度器
     */
    bool notify() {
        // 防止重复通知
        bool expected = false;
        if (!m_ready.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return false;
        }

        // 唤醒等待的协程（如果有）
        expected = true;
        if (m_waiting.compare_exchange_strong(expected, false,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            m_waker.wakeUp();
        }
        return true;
    }

    /**
     * @brief 检查是否已有协程在等待
     * @return true 当前已有协程注册在该等待器上并处于挂起状态
     */
    bool isWaiting() const {
        return m_waiting.load(std::memory_order_acquire);
    }

    /**
     * @brief 检查完成信号是否已经到达
     * @return true 已调用 notify() 且结果对等待侧可见
     */
    bool isReady() const {
        return m_ready.load(std::memory_order_acquire);
    }

private:
    friend class AsyncWaiterAwaitable<void>;

    std::atomic<bool> m_waiting{false};
    std::atomic<bool> m_ready{false};
    Waker m_waker;
};

// AsyncWaiterAwaitable<T> 实现
template<typename T>
bool AsyncWaiterAwaitable<T>::await_ready() const noexcept {
    return m_waiter->m_ready.load(std::memory_order_acquire);
}

template<typename T>
template <typename Promise>
bool AsyncWaiterAwaitable<T>::await_suspend(std::coroutine_handle<Promise> handle) noexcept {
    m_waiter->m_waker = Waker(handle);

    // 设置等待状态
    bool expected = false;
    if (!m_waiter->m_waiting.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
        // 已经有人在等待，或者已经 notify 过
        return false;
    }

    // 再次检查，防止在设置等待状态期间已经 notify
    if (m_waiter->m_ready.load(std::memory_order_acquire)) {
        m_waiter->m_waiting.store(false, std::memory_order_release);
        return false;  // 不挂起，直接返回
    }
    return true;  // 挂起
}

template<typename T>
std::expected<T, IOError> AsyncWaiterAwaitable<T>::await_resume() noexcept {
    if (m_waiter->m_result.has_value()) {
        return std::move(m_waiter->m_result.value());
    }
    return std::unexpected(IOError(kTimeout, 0));
}

// AsyncWaiterAwaitable<void> 实现
inline bool AsyncWaiterAwaitable<void>::await_ready() const noexcept {
    return m_waiter->m_ready.load(std::memory_order_acquire);
}

template <typename Promise>
inline bool AsyncWaiterAwaitable<void>::await_suspend(std::coroutine_handle<Promise> handle) noexcept {
    m_waiter->m_waker = Waker(handle);
    bool expected = false;
    if (!m_waiter->m_waiting.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
        return false;
    }

    if (m_waiter->m_ready.load(std::memory_order_acquire)) {
        m_waiter->m_waiting.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

} // namespace galay::kernel

#endif // GALAY_KERNEL_ASYNC_WAITER_H
