#ifndef GALAY_ASYNC_WAITER_HPP
#define GALAY_ASYNC_WAITER_HPP

#include "Result.hpp"
#include "galay/common/Common.h"

namespace galay
{
    class AsyncWaiter;
    template<CoType T>
    class AsyncResultWaiter;

    namespace details
    {
        class WaitEvent: public AsyncEvent<nil>
        {
        public:
            WaitEvent(AsyncWaiter& waiter);
            //return true while not suspend
            bool ready() override;
            //return true while suspend
            bool suspend(Waker waker) override;
        private:
            AsyncWaiter& m_waiter;
        };

        template<CoType T>
        class ResultWaitEvent: public AsyncEvent<T>
        {
            template<CoType M>
            friend class galay::AsyncResultWaiter;
        public:
            ResultWaitEvent(AsyncResultWaiter<T>& waiter);
            //return true while not suspend
            bool ready() override;
            //return true while suspend
            bool suspend(Waker waker) override;
        private:
            AsyncResultWaiter<T>& m_waiter;
        };
    }

    class AsyncWaiter 
    {
        friend class details::WaitEvent;
    public:
        AsyncWaiter();
        AsyncResult<nil> wait();
        void notify();
    private:
        Waker m_waker;
        std::atomic_bool m_wait = false;
        std::shared_ptr<details::WaitEvent> m_event;
    };

    template<CoType T>
    class AsyncResultWaiter 
    {
        template<CoType M>
        friend class details::ResultWaitEvent;
    public:
        AsyncResultWaiter();
        AsyncResult<T> wait();
        void notify(T&& value);
    private:
        Waker m_waker;
        std::atomic_bool m_wait = false;
        std::shared_ptr<details::ResultWaitEvent<T>> m_event;
    };


    template <CoType T>
    inline AsyncResultWaiter<T>::AsyncResultWaiter()
        : m_event(std::make_shared<details::ResultWaitEvent<T>>(*this))
    {
    }

    template <CoType T>
    inline AsyncResult<T> AsyncResultWaiter<T>::wait()
    {
        return {m_event};
    }

    template <CoType T>
    inline void AsyncResultWaiter<T>::notify(T &&value)
    {
        if(m_wait.load()) {
            m_wait.store(false);
            m_event->m_result = std::move(value);
            m_waker.wakeUp();
        }
    }
}

namespace galay::details 
{
    template <CoType T>
    inline ResultWaitEvent<T>::ResultWaitEvent(AsyncResultWaiter<T> &waiter)
        : m_waiter(waiter)
    {
    }
    
    template <CoType T>
    inline bool ResultWaitEvent<T>::ready()
    {
        return false;
    }

    template <CoType T>
    inline bool ResultWaitEvent<T>::suspend(Waker waker)
    {
        m_waiter.m_waker = waker;
        m_waiter.m_wait.store(true);
        return true;
    }
}

#endif