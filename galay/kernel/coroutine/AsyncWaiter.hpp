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
        class WaitEvent: public AsyncEvent<ValueWrapper<nil>>
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
        class ResultWaitEvent: public AsyncEvent<ValueWrapper<T>>
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
        AsyncResult<ValueWrapper<nil>> wait();
        bool isWaiting();
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
        AsyncResult<ValueWrapper<T>> wait();
        bool isWaiting();
        void notify(T&& value);
        void notify(ValueWrapper<T>&& value);
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
    inline AsyncResult<ValueWrapper<T>> AsyncResultWaiter<T>::wait()
    {
        return {m_event};
    }

    template <CoType T>
    inline bool AsyncResultWaiter<T>::isWaiting()
    {
        return m_wait.load();
    }

    template <CoType T>
    inline void AsyncResultWaiter<T>::notify(T &&value)
    {
        bool expected = true;
        if(m_wait.compare_exchange_strong(expected, false, 
                                      std::memory_order_acq_rel, 
                                      std::memory_order_acquire)) {
            makeValue(m_event->m_result, std::move(value), nullptr);
            m_waker.wakeUp();
        }
    }

    template <CoType T>
    inline void AsyncResultWaiter<T>::notify(ValueWrapper<T> &&value)
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
        bool expected = false;
        if(!m_waiter.m_wait.compare_exchange_strong(expected, true, 
                                              std::memory_order_acq_rel, 
                                              std::memory_order_acquire)) {
            using namespace error;
            SystemError::ptr e = std::make_shared<SystemError>(ConcurrentError, errno);
            makeValue(this->m_result, e);
            return false;
        }
        return true;
    }
}

#endif