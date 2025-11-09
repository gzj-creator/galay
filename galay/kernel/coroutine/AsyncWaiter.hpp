#ifndef GALAY_ASYNC_WAITER_HPP
#define GALAY_ASYNC_WAITER_HPP

#include "Result.hpp"
#include "galay/common/Common.h"
#include "CoScheduler.hpp"
#include <list>

namespace galay
{
    template<typename T, typename E>
    class AsyncWaiter;

    namespace details
    {
        template<typename T, typename E>
        class WaitEvent: public AsyncEvent<std::expected<T, E>>
        {
            template<typename M, typename F>
            friend class galay::AsyncWaiter;
        public:
            WaitEvent(AsyncWaiter<T, E>& waiter);
            //return true while not suspend
            bool onReady() override;
            //return true while suspend
            bool onSuspend(Waker waker) override;

            template<CoType Type>
            void appendTask(Coroutine<Type>&& co);
        private:
            AsyncWaiter<T, E>& m_waiter;
            std::shared_ptr<std::list<CoroutineBase::wptr>> m_tasks;
        };
    }

    template<typename T, typename E>
    class AsyncWaiter 
    {
        template<typename M, typename F>
        friend class details::WaitEvent;
    public:
        AsyncWaiter();
        template<CoType Type>
        void appendTask(Coroutine<Type>&& co);
        AsyncResult<std::expected<T, E>> wait();
        bool isWaiting();
        bool notify(std::expected<T, E>&& value);
    private:
        Waker m_waker;
        std::atomic_bool m_wait = false;
        std::shared_ptr<details::WaitEvent<T, E>> m_event;
    };

    template <typename T, typename E>
    inline AsyncWaiter<T, E>::AsyncWaiter()
        : m_event(std::make_shared<details::WaitEvent<T, E>>(*this))
    {
    }

    template <typename T, typename E>
    inline AsyncResult<std::expected<T, E>> AsyncWaiter<T, E>::wait()
    {
        return {this->m_event};
    }

    template <typename T, typename E>
    inline bool AsyncWaiter<T, E>::isWaiting()
    {
        return m_wait.load();
    }

    template <typename T, typename E>
    inline bool AsyncWaiter<T, E>::notify(std::expected<T, E> &&value)
    {
        bool expected = true;
        if(m_wait.compare_exchange_strong(expected, false, 
                                      std::memory_order_acq_rel, 
                                      std::memory_order_acquire)) {
            m_event->m_result = std::move(value);
            m_waker.wakeUp();
            return true;
        }
        return false;
    }


    template <typename T, typename E>
    template <CoType Type>
    inline void AsyncWaiter<T, E>::appendTask(Coroutine<Type> &&co)
    {
        m_event->appendTask(std::move(co));
    }
}

namespace galay::details 
{
    template <typename T, typename E>
    inline WaitEvent<T, E>::WaitEvent(AsyncWaiter<T, E> &waiter)
        : m_waiter(waiter)
    {
    }
    
    template <typename T, typename E>
    inline bool WaitEvent<T, E>::onReady()
    {
        return false;
    }

    template <typename T, typename E>
    inline bool WaitEvent<T, E>::onSuspend(Waker waker)
    {
        m_waiter.m_waker = waker;
        bool expected = false;
        if(!m_waiter.m_wait.compare_exchange_strong(expected, true, 
                                              std::memory_order_acq_rel, 
                                              std::memory_order_acquire)) {
            LogTrace("WaitEvent::suspend: waiter already set");
            return false;
        }
        if(m_tasks) {
            for(auto it = m_tasks->begin(); it != m_tasks->end(); ++it) {
                waker.belongScheduler()->schedule(*it);
            }
        }
        return true;
    }

    template <typename T, typename E>
    template <CoType Type>
    inline void WaitEvent<T, E>::appendTask(Coroutine<Type> &&co)
    {
        if(!m_tasks) {
            m_tasks = std::make_shared<std::list<CoroutineBase::wptr>>();
        }
        m_tasks->emplace_back(co.origin());
    }
}

#endif