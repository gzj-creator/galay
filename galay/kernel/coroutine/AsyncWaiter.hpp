#ifndef GALAY_ASYNC_WAITER_HPP
#define GALAY_ASYNC_WAITER_HPP

#include "Result.hpp"
#include "galay/common/Common.h"

namespace galay
{
    template<typename E>
    class AsyncWaiter;
    template<CoType T, typename E>
    class AsyncResultWaiter;

    namespace details
    {
        template<typename E>
        class WaitEvent: public AsyncEvent<std::expected<void, E>>
        {
        public:
            WaitEvent(AsyncWaiter<E>& waiter);
            //return true while not suspend
            bool ready() override;
            //return true while suspend
            bool suspend(Waker waker) override;
        private:
            AsyncWaiter<E>& m_waiter;
        };

        template<CoType T, typename E>
        class ResultWaitEvent: public AsyncEvent<std::expected<T, CommonError>>
        {
            template<CoType M, typename F>
            friend class galay::AsyncResultWaiter;
        public:
            ResultWaitEvent(AsyncResultWaiter<T, E>& waiter);
            //return true while not suspend
            bool ready() override;
            //return true while suspend
            bool suspend(Waker waker) override;
        private:
            AsyncResultWaiter<T, E>& m_waiter;
        };
    }

    template<typename E>
    class AsyncWaiter 
    {
        template<typename F>
        friend class details::WaitEvent;
    public:
        AsyncWaiter();
        AsyncResult<std::expected<void, E>> wait();
        bool isWaiting();
        void notify();
    private:
        Waker m_waker;
        std::atomic_bool m_wait = false;
        std::shared_ptr<details::WaitEvent<E>> m_event;
    };

    template<CoType T, typename E>
    class AsyncResultWaiter 
    {
        template<CoType M, typename F>
        friend class details::ResultWaitEvent;
    public:
        AsyncResultWaiter();
        AsyncResult<std::expected<T, E>> wait();
        bool isWaiting();
        void notify(std::expected<T, E>&& value);
    private:
        Waker m_waker;
        std::atomic_bool m_wait = false;
        std::shared_ptr<details::ResultWaitEvent<T, E>> m_event;
    };

    template<typename E>
    AsyncWaiter<E>::AsyncWaiter()
        :m_event(std::make_shared<details::WaitEvent<E>>(*this))
    {
    }

    template<typename E>
    AsyncResult<std::expected<void, E>> AsyncWaiter<E>::wait()
    {
        return {m_event};
    }

    template<typename E>
    bool AsyncWaiter<E>::isWaiting()
    {
        return m_wait.load();
    }

    template<typename E>
    void AsyncWaiter<E>::notify()
    {
        bool expected = true;
        if(m_wait.compare_exchange_strong(expected, false, 
                                      std::memory_order_acq_rel, 
                                      std::memory_order_acquire)) {
            m_waker.wakeUp();
        }
    }



    template <CoType T, typename E>
    inline AsyncResultWaiter<T, E>::AsyncResultWaiter()
        : m_event(std::make_shared<details::ResultWaitEvent<T, E>>(*this))
    {
    }

    template <CoType T, typename E>
    inline AsyncResult<std::expected<T, E>> AsyncResultWaiter<T, E>::wait()
    {
        return {this->m_event};
    }

    template <CoType T, typename E>
    inline bool AsyncResultWaiter<T, E>::isWaiting()
    {
        return m_wait.load();
    }

    template <CoType T, typename E>
    inline void AsyncResultWaiter<T, E>::notify(std::expected<T, E> &&value)
    {
        bool expected = true;
        if(m_wait.compare_exchange_strong(expected, false, 
                                      std::memory_order_acq_rel, 
                                      std::memory_order_acquire)) {
            m_event->m_result = std::move(value);
            m_waker.wakeUp();
        }
    }
}

namespace galay::details 
{
    template<typename E>
    WaitEvent<E>::WaitEvent(AsyncWaiter<E> &waiter)
        : m_waiter(waiter)
    {
    }

    template<typename E>
    bool WaitEvent<E>::ready()
    {
        return false;
    }

    template<typename E>
    bool WaitEvent<E>::suspend(Waker waker)
    {
        m_waiter.m_waker = waker;
        bool expected = false;
        if(!m_waiter.m_wait.compare_exchange_strong(expected, true, 
                                      std::memory_order_acq_rel, 
                                      std::memory_order_acquire)) {
            LogTrace("ResultWaitEvent::suspend: waiter already set");
            return false;
        }
        return true;
    }

    template <CoType T, typename E>
    inline ResultWaitEvent<T, E>::ResultWaitEvent(AsyncResultWaiter<T, E> &waiter)
        : m_waiter(waiter)
    {
    }
    
    template <CoType T, typename E>
    inline bool ResultWaitEvent<T, E>::ready()
    {
        return false;
    }

    template <CoType T, typename E>
    inline bool ResultWaitEvent<T, E>::suspend(Waker waker)
    {
        m_waiter.m_waker = waker;
        bool expected = false;
        if(!m_waiter.m_wait.compare_exchange_strong(expected, true, 
                                              std::memory_order_acq_rel, 
                                              std::memory_order_acquire)) {
            LogTrace("ResultWaitEvent::suspend: waiter already set");
            return false;
        }
        return true;
    }
}

#endif