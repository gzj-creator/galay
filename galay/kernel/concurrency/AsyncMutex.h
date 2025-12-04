#ifndef GALAY_KERNEL_ASYNC_ASYNCMUTEX_H
#define GALAY_KERNEL_ASYNC_ASYNCMUTEX_H

#include <atomic>
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include "galay/kernel/coroutine/AsyncEvent.hpp"
#include "galay/kernel/coroutine/Result.hpp"
#include "galay/kernel/coroutine/Waker.h"

namespace galay
{
    class AsyncMutex;

    namespace details
    {
        class LockEvent : public AsyncEvent<nil>
        {
            friend class galay::AsyncMutex;
        public:
            LockEvent(AsyncMutex& mutex);
            bool onReady() override;
            bool onSuspend(Waker waker) override;

        private:
            AsyncMutex& m_mutex;
        };
    }

    class AsyncMutex
    {
        friend class details::LockEvent;

    public:
        AsyncMutex();
        ~AsyncMutex();

        // 返回AsyncResult，协程可以await
        AsyncResult<nil> lock();

        // 解锁
        void unlock();

        // 检查是否已被锁定
        bool isLocked() const;

    private:
        std::atomic_bool m_locked{false};

        // 无锁等待队列 - 存储<Waker, LockEvent*>对
        // LockEvent*用于标记该协程是否已被唤醒
        moodycamel::ConcurrentQueue<Waker> m_waiters;

        // 尝试获取锁，返回是否成功
        bool tryLock();

        // 唤醒下一个等待的协程
        void wakeupNext();
    };

}

#include "AsyncMutex.inl"

#endif // GALAY_KERNEL_ASYNC_ASYNCMUTEX_H
