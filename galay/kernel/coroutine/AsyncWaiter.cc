#include "AsyncWaiter.hpp"

namespace galay 
{
    details::WaitEvent::WaitEvent(AsyncWaiter &waiter)
        : m_waiter(waiter)
    {
    }

    bool details::WaitEvent::ready()
    {
        return false;
    }

    bool details::WaitEvent::suspend(Waker waker)
    {
        m_waiter.m_waker = waker;
        bool expected = false;
        if(!m_waiter.m_wait.compare_exchange_strong(expected, true, 
                                      std::memory_order_acq_rel, 
                                      std::memory_order_acquire)) {
            using namespace error;
            SystemError::ptr e = std::make_shared<SystemError>(ConcurrentError, errno);
            makeValue(m_result, e);
            return false;
        }
        return true;
    }

    AsyncWaiter::AsyncWaiter()
        :m_event(std::make_shared<details::WaitEvent>(*this))
    {
    }

    AsyncResult<ValueWrapper<nil>> AsyncWaiter::wait()
    {
        return {m_event};
    }

    bool AsyncWaiter::isWaiting()
    {
        return m_wait.load();
    }

    void AsyncWaiter::notify()
    {
        bool expected = true;
        if(m_wait.compare_exchange_strong(expected, false, 
                                      std::memory_order_acq_rel, 
                                      std::memory_order_acquire)) {
            m_waker.wakeUp();
        }
    }

    
}
