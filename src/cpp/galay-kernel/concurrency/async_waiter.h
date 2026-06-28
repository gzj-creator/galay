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

#include "../core/task.h"
#include "../core/timeout.hpp"
#include "../core/waker.h"
#include "../common/error.h"
#include <atomic>
#include <concepts>
#include <optional>
#include <expected>
#include <coroutine>
#include <type_traits>
#include <cstdint>

namespace galay::kernel
{

enum class AsyncWaiterState : uint8_t {
    kEmpty,
    kWaiting,
    kReady,
};

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
    bool await_suspend(Waker waker) noexcept;  ///< 使用外部 Waker 注册等待，用于 C coroutine bridge
    std::expected<void, IOError> await_resume() noexcept;  ///< 返回完成结果；超时时返回 IOError(kTimeout, 0)
    void markTimeout() noexcept;  ///< 标记超时并清理等待器中的外部 Waker

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
        bool expected = false;
        if (!m_notified.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            return false;
        }

        m_result = std::move(result);
        m_ready.store(true, std::memory_order_release);

        const auto previous = m_state.exchange(AsyncWaiterState::kReady,
                                               std::memory_order_acq_rel);
        if (previous == AsyncWaiterState::kWaiting) {
            m_waker.wakeUp();
        }
        return true;
    }

    /**
     * @brief 检查是否正在等待
     * @return true 当前已有协程注册在该等待器上并处于挂起状态
     */
    bool isWaiting() const {
        return m_state.load(std::memory_order_acquire) == AsyncWaiterState::kWaiting;
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
    std::atomic<AsyncWaiterState> m_state{AsyncWaiterState::kEmpty};  ///< 等待状态
    std::atomic<bool> m_notified{false};        ///< 是否已经通知过
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
        bool expected = false;
        if (!m_notified.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            return false;
        }

        m_ready.store(true, std::memory_order_release);
        const auto previous = m_state.exchange(AsyncWaiterState::kReady,
                                               std::memory_order_acq_rel);
        if (previous == AsyncWaiterState::kWaiting) {
            m_waker.wakeUp();
        }
        return true;
    }

    /**
     * @brief 检查是否已有协程在等待
     * @return true 当前已有协程注册在该等待器上并处于挂起状态
     */
    bool isWaiting() const {
        return m_state.load(std::memory_order_acquire) == AsyncWaiterState::kWaiting;
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

    std::atomic<AsyncWaiterState> m_state{AsyncWaiterState::kEmpty};
    std::atomic<bool> m_notified{false};
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
    auto* waiter = m_waiter;
    waiter->m_waker = Waker(handle);

    if (waiter->m_ready.load(std::memory_order_acquire)) {
        return false;
    }

    AsyncWaiterState expected = AsyncWaiterState::kEmpty;
    if (waiter->m_state.compare_exchange_strong(expected,
                                                AsyncWaiterState::kWaiting,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        return true;
    }
    return false;
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
    auto* waiter = m_waiter;
    waiter->m_waker = Waker(handle);
    if (waiter->m_ready.load(std::memory_order_acquire)) {
        return false;
    }

    AsyncWaiterState expected = AsyncWaiterState::kEmpty;
    if (waiter->m_state.compare_exchange_strong(expected,
                                                AsyncWaiterState::kWaiting,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        return true;
    }
    return false;
}

inline bool AsyncWaiterAwaitable<void>::await_suspend(Waker waker) noexcept {
    auto* waiter = m_waiter;
    waiter->m_waker = std::move(waker);
    if (waiter->m_ready.load(std::memory_order_acquire)) {
        return false;
    }

    AsyncWaiterState expected = AsyncWaiterState::kEmpty;
    if (waiter->m_state.compare_exchange_strong(expected,
                                                AsyncWaiterState::kWaiting,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        return true;
    }
    return false;
}

inline void AsyncWaiterAwaitable<void>::markTimeout() noexcept {
    m_result = std::unexpected(IOError(kTimeout, 0));
    AsyncWaiterState expected = AsyncWaiterState::kWaiting;
    if (m_waiter->m_state.compare_exchange_strong(expected,
                                                  AsyncWaiterState::kEmpty,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
        m_waiter->m_waker = Waker();
    }
}

inline std::expected<void, IOError> AsyncWaiterAwaitable<void>::await_resume() noexcept {
    m_waiter->m_waker = Waker();
    return m_result;
}

} // namespace galay::kernel

#endif // GALAY_KERNEL_ASYNC_WAITER_H
