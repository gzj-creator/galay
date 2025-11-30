#ifndef GALAY_KERNEL_ASYNC_ASYNCMUTEX_INL
#define GALAY_KERNEL_ASYNC_ASYNCMUTEX_INL

#include "AsyncMutex.h"

namespace galay
{
    namespace details
    {
        inline LockEvent::LockEvent(AsyncMutex& mutex)
            : m_mutex(mutex)
        {
        }

        inline bool LockEvent::onReady()
        {
            // 检查是否能立即获取锁
            return m_mutex.tryLock();
        }

        inline bool LockEvent::onSuspend(Waker waker)
        {
            // Step 1: 无锁入队
            m_mutex.m_waiters.enqueue(waker);

            // Step 2: Double-Check - 再次尝试获取锁
            if (m_mutex.tryLock())
            {
                // 成功获取锁，从队列中移除当前 waker
                Waker w;
                if (m_mutex.m_waiters.try_dequeue(w))
                {
                    // w 就是当前协程（或至少是队列中的一个）
                    // 由于我们成功 tryLock，不需要暂停
                }
                return false; // 不需要真正暂停，直接返回
            }

            // 需要暂停，等待被唤醒
            return true;
        }
    }

    inline AsyncMutex::AsyncMutex()
        : m_locked(0)
    {
    }

    inline AsyncMutex::~AsyncMutex()
    {
    }

    inline AsyncResult<nil> AsyncMutex::lock()
    {
        // 尝试立即获取锁
        if (tryLock())
        {
            return AsyncResult<nil>(nil{});
        }

        // 创建 LockEvent，协程将在此处暂停
        auto event = std::make_shared<details::LockEvent>(*this);
        return AsyncResult<nil>(event);
    }

    inline void AsyncMutex::unlock()
    {
        // 标记为未锁定 (Release 语义)
        m_locked.store(0, std::memory_order_release);

        // 唤醒下一个等待的协程
        wakeupNext();
    }

    inline bool AsyncMutex::isLocked() const
    {
        return m_locked.load(std::memory_order_acquire) != 0;
    }

    inline bool AsyncMutex::tryLock()
    {
        uint32_t expected = 0;
        return m_locked.compare_exchange_strong(
            expected, 1,
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
            }
            // 如果获取锁失败，继续尝试下一个等待者
        }
    }
}

#endif // GALAY_KERNEL_ASYNC_ASYNCMUTEX_INL

