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
        m_waiter.m_wait.store(true);
        return true;
    }

    AsyncWaiter::AsyncWaiter()
        :m_event(std::make_shared<details::WaitEvent>(*this))
    {
    }

    AsyncResult<nil> AsyncWaiter::wait()
    {
        return {m_event};
    }

    void AsyncWaiter::notify()
    {
        if(m_wait.load()) {
            m_wait.store(false);
            m_waker.wakeUp();
        }
    }

    
}
