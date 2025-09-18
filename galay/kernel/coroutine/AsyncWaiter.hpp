#ifndef GALAY_ASYNC_WAITER_HPP
#define GALAY_ASYNC_WAITER_HPP

#include "Result.hpp"
#include "galay/common/Common.h"

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
        private:
            AsyncWaiter<T, E>& m_waiter;
        };

        template<typename E>
        class WaitEvent<void, E>: public AsyncEvent<std::expected<void, E>>
        {
            template<typename M, typename F>
            friend class galay::AsyncWaiter;
        public:
            WaitEvent(AsyncWaiter<void, E>& waiter);
            //return true while not suspend
            bool onReady() override;
            //return true while suspend
            bool onSuspend(Waker waker) override;
        private:
            AsyncWaiter<void, E>& m_waiter;
        };

    }

    template<typename T, typename E>
    class AsyncWaiter 
    {
        template<typename M, typename F>
        friend class details::WaitEvent;
    public:
        AsyncWaiter();
        AsyncResult<std::expected<T, E>> wait();
        bool isWaiting();
        bool notify(std::expected<T, E>&& value);
    private:
        Waker m_waker;
        std::atomic_bool m_wait = false;
        std::shared_ptr<details::WaitEvent<T, E>> m_event;
    };

    template<typename E>
    class AsyncWaiter<void, E>
    {
        template<typename M, typename F>
        friend class details::WaitEvent;
    public:
        AsyncWaiter();
        AsyncResult<std::expected<void, E>> wait();
        bool isWaiting();
        bool notify();
    private:
        Waker m_waker;
        std::atomic_bool m_wait = false;
        std::shared_ptr<details::WaitEvent<void, E>> m_event;
    };


    template<typename E>
    AsyncWaiter<void, E>::AsyncWaiter()
        :m_event(std::make_shared<details::WaitEvent<void, E>>(*this))
    {
    }

    template<typename E>
    AsyncResult<std::expected<void, E>> AsyncWaiter<void, E>::wait()
    {
        return {m_event};
    }

    template<typename E>
    bool AsyncWaiter<void, E>::isWaiting()
    {
        return m_wait.load();
    }

    template<typename E>
    bool AsyncWaiter<void, E>::notify()
    {
        bool expected = true;
        if(m_wait.compare_exchange_strong(expected, false, 
                                      std::memory_order_acq_rel, 
                                      std::memory_order_acquire)) {
            m_waker.wakeUp();
            return true;
        }
        return false;
    }



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
}

namespace galay::details 
{
    template<typename E>
    WaitEvent<void, E>::WaitEvent(AsyncWaiter<void, E> &waiter)
        : m_waiter(waiter)
    {
    }

    template<typename E>
    bool WaitEvent<void, E>::onReady()
    {
        return false;
    }

    template<typename E>
    bool WaitEvent<void, E>::onSuspend(Waker waker)
    {
        m_waiter.m_waker = waker;
        bool expected = false;
        if(!m_waiter.m_wait.compare_exchange_strong(expected, true, 
                                      std::memory_order_acq_rel, 
                                      std::memory_order_acquire)) {
            LogTrace("WaitEvent::suspend: waiter already set");
            return false;
        }
        return true;
    }

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
        return true;
    }
}

#endif