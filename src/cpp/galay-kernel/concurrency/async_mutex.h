/**
 * @file async_mutex.h
 * @brief 异步互斥锁
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供协程友好的互斥锁。当锁被占用时，调用方会挂起当前协程，
 * 而不是阻塞底层线程。`lock()` / `lock().timeout(...)` 的返回值均为
 * `std::expected<void, IOError>`：成功时为 value，超时或失败时为 error。
 *
 * 特性：
 * - 无锁等待队列（基于 moodycamel::ConcurrentQueue）
 * - 协程友好，支持 co_await
 * - 公平性：FIFO 顺序唤醒等待协程
 * - 线程安全
 * - 支持超时
 * - timeout / 直接抢锁成功后会清理 waiter，避免残留唤醒
 *
 * 使用方式：
 * @code
 * AsyncMutex mutex;
 *
 * galay::kernel::Task<void> task() {
 *     auto locked = co_await mutex.lock();
 *     if (!locked) {
 *         co_return;
 *     }
 *     // 临界区
 *     mutex.unlock();
 *     co_return;
 * }
 *
 * // 使用超时
 * galay::kernel::Task<void> task2() {
 *     auto locked = co_await mutex.lock().timeout(100ms);
 *     if (locked) {
 *         // 临界区
 *         mutex.unlock();
 *     } else if (galay::kernel::IOError::contains(locked.error().code(), galay::kernel::kTimeout)) {
 *         // 超时
 *     }
 *     co_return;
 * }
 * @endcode
 */

#ifndef GALAY_KERNEL_ASYNC_MUTEX_H
#define GALAY_KERNEL_ASYNC_MUTEX_H

#include "../core/task.h"
#include "../core/timeout.hpp"
#include "../core/waker.h"
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <atomic>
#include <coroutine>
#include <memory>

namespace galay::kernel
{

class AsyncMutex;
class AsyncLockGuard;

struct AsyncMutexWaiter
{
    explicit AsyncMutexWaiter(Waker waiter_waker)
        : waker(std::move(waiter_waker))
    {
    }

    Waker waker;
    std::atomic<bool> active{true};
};


/**
 * @brief AsyncMutex 的等待体
 */
class AsyncMutexAwaitable : public TimeoutSupport<AsyncMutexAwaitable>
{
public:
    explicit AsyncMutexAwaitable(AsyncMutex* mutex) : m_mutex(mutex) {}  ///< 构造与指定互斥锁绑定的等待体

    bool await_ready() const noexcept;  ///< 若当前可直接抢锁则返回 true，避免挂起
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;  ///< 注册 waiter，并在未抢到锁时挂起当前协程
    std::expected<void, IOError> await_resume() noexcept
    {
        cancelWaiter();
        m_waiter.reset();
        return m_result;
    }

private:
    friend struct WithTimeout<AsyncMutexAwaitable>;
    void markTimeout() noexcept;  ///< 标记等待超时，并让残留 waiter 在 unlock() 时失效
    void cancelWaiter() noexcept;  ///< 让当前 waiter 失效，防止后续陈旧唤醒

    AsyncMutex* m_mutex;
    std::expected<void, IOError> m_result;
    std::shared_ptr<AsyncMutexWaiter> m_waiter;
};

/**
 * @brief 异步互斥锁
 *
 * @details Task 友好的互斥锁实现，使用无锁队列管理等待任务。
 * 当锁被占用时，调用 lock() 的任务会被挂起并加入等待队列，
 * 而不是阻塞当前线程。unlock() 会唤醒队列中的下一个等待任务。
 *
 * 实现细节：
 * - 使用 atomic<bool> 管理锁状态
 * - 使用 moodycamel::ConcurrentQueue 作为无锁等待队列
 *
 * @note 线程安全，无锁实现
 */
class AsyncMutex
{
public:
    /**
     * @brief 构造函数
     * @param initial_capacity 等待队列初始容量，默认 32
     */
    explicit AsyncMutex(size_t initial_capacity = 32)
        : m_waiters(initial_capacity) {}

    // 禁止拷贝和移动
    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    AsyncMutex(AsyncMutex&&) = delete;
    AsyncMutex& operator=(AsyncMutex&&) = delete;

    /**
     * @brief 异步获取锁。
     * @return 可等待对象；`co_await` 成功时返回 value，timeout 时返回 `IOError(kTimeout, ...)`
     *
     * @note
     * - 不会阻塞底层线程，只会挂起当前协程
     * - waiter 在 timeout 或入队后直接抢锁成功时会自动失效，避免残留唤醒
     */
    AsyncMutexAwaitable lock() {
        return AsyncMutexAwaitable(this);
    }

    /**
     * @brief 释放锁并唤醒一个仍然有效的等待协程。
     *
     * @note
     * - 必须仅由当前持锁路径调用
     * - 如果队列中的 waiter 已 timeout 或已失效，会被跳过而不会重新占锁
     */
    void unlock() {
        m_locked.store(false, std::memory_order_release);
        std::shared_ptr<AsyncMutexWaiter> waiter;
        while (m_waiters.try_dequeue(waiter))
        {
            if (!waiter) {
                continue;
            }

            bool expected = true;
            if (!waiter->active.compare_exchange_strong(expected, false,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
                continue;
            }

            // 尝试获取锁
            if (tryLock())
            {
                // 成功获取锁，唤醒该协程
                waiter->waker.wakeUp();
                return;
            } else {
                // 在释放与唤醒之间如果锁被其他协程重新获取，恢复 waiter 并重新排队。
                waiter->active.store(true, std::memory_order_release);
                m_waiters.enqueue(std::move(waiter));
                return;
            }
        }
    }

    /**
     * @brief 观察当前锁状态。
     * @return `true` 表示当前有路径持锁；该结果只适合诊断/断言，不应用作无竞争同步条件
     */
    bool isLocked() const {
        return m_locked.load(std::memory_order_acquire);
    }

private:
    friend class AsyncMutexAwaitable;
    friend class AsyncScopedLockAwaitable;

    /**
     * @brief 尝试获取锁（非阻塞，仅内部使用）
     * @return true 成功获取锁，false 锁被占用
     */
    bool tryLock() {
        bool expected = false;
        return m_locked.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
    }

    std::atomic<bool> m_locked{false};                      ///< 锁状态
    moodycamel::ConcurrentQueue<std::shared_ptr<AsyncMutexWaiter>> m_waiters;  ///< 无锁等待队列
};

// AsyncMutexAwaitable 实现
inline bool AsyncMutexAwaitable::await_ready() const noexcept {
    if (m_mutex->tryLock()) {
        const_cast<AsyncMutexAwaitable*>(this)->m_result = {};
        return true;
    }
    return false;
}

template <typename Promise>
inline bool AsyncMutexAwaitable::await_suspend(std::coroutine_handle<Promise> handle) noexcept {
    m_waiter = std::make_shared<AsyncMutexWaiter>(Waker(handle));
    m_mutex->m_waiters.enqueue(m_waiter);
    // 再次检查，防止在入队期间锁被释放
    if (m_mutex->tryLock()) {
        cancelWaiter();
        m_result = {};
        return false;  // 不挂起
    }
    return true;
}

inline void AsyncMutexAwaitable::markTimeout() noexcept {
    cancelWaiter();
    m_result = std::unexpected(IOError(kTimeout, 0));
}

inline void AsyncMutexAwaitable::cancelWaiter() noexcept {
    if (m_waiter) {
        m_waiter->active.store(false, std::memory_order_release);
    }
}
} // namespace galay::kernel

#endif // GALAY_KERNEL_ASYNC_MUTEX_H
