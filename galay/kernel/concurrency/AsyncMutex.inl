#ifndef GALAY_KERNEL_ASYNC_ASYNCMUTEX_INL
#define GALAY_KERNEL_ASYNC_ASYNCMUTEX_INL

#include "AsyncMutex.h"
#include <atomic>

namespace galay
{
    namespace details
    {
        inline LockResult::LockResult(AsyncMutex& mutex)
            : m_mutex(mutex)
        {
        }

        inline bool LockResult::await_ready()
        {
            // 检查是否能立即获取锁
            return m_mutex.tryLock();
        }

        inline bool LockResult::await_suspend(std::coroutine_handle<> handle)
        {
            m_wait_co = std::coroutine_handle<PromiseTypeBase>::from_address(handle.address()).promise().getCoroutine();
            if(auto co_ptr = m_wait_co.lock()) {
                co_ptr->modToSuspend();
                m_mutex.m_waiters.enqueue(Waker(m_wait_co));
                return true;
            }
            return false;
        }

        inline nil LockResult::await_resume() const
        {
            if(auto co_ptr = m_wait_co.lock()) {
                co_ptr->modToRunning();
            }
            return nil{};
        }
    }

    inline AsyncMutex::AsyncMutex()
        : m_locked(false)
    {
    }

    inline AsyncMutex::~AsyncMutex()
    {
    }

    inline details::LockResult AsyncMutex::lock()
    {
        return details::LockResult(*this);
    }       

    inline void AsyncMutex::unlock()
    {
        // 标记为未锁定 (Release 语义)
        m_locked.store(false, std::memory_order_release);

        // 唤醒下一个等待的协程
        wakeupNext();
    }

    inline bool AsyncMutex::isLocked() const
    {
        return m_locked.load(std::memory_order_acquire) != 0;
    }

    inline bool AsyncMutex::tryLock()
    {
        bool expected = false;
        return m_locked.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        );
    }

    inline void AsyncMutex::wakeupNext()
    {
        Waker waker;
        while (m_waiters.try_dequeue(waker))
        {
            // 尝试获取锁
            if (tryLock())
            {
                // 成功获取锁，唤醒该协程
                waker.wakeUp();
                return;
            } else {
                // 说明在 unlock  ----   dequeue时间内有lock
                m_waiters.enqueue(waker);
                return;
            }
        }
    }
}

#endif // GALAY_KERNEL_ASYNC_ASYNCMUTEX_INL

