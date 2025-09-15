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
            m_result = std::unexpected(CommonError(ConcurrentError, static_cast<uint32_t>(errno)));
            return false;
        }
        return true;
    }

    AsyncWaiter::AsyncWaiter()
        :m_event(std::make_shared<details::WaitEvent>(*this))
    {
    }

    AsyncResult<std::expected<void, CommonError>> AsyncWaiter::wait()
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
