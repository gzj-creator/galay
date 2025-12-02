#ifndef GALAY_RESULT_INL
#define GALAY_RESULT_INL


#include "Result.hpp"
#include "CoScheduler.hpp"

namespace galay {

    template<CoType T>
    inline AsyncResult<T>::AsyncResult(typename AsyncEvent<T>::ptr event)
        : m_event(event)
    {
    }

    template <CoType T>
    inline AsyncResult<T>::AsyncResult(T &&value)
        : m_event(std::make_shared<ResultEvent<T>>(std::move(value)))
    {
    }

    template<CoType T>
    inline bool AsyncResult<T>::await_ready()
    {
        if(!m_event) return true;
        return m_event->onReady();
    }

    template<CoType T>
    inline bool AsyncResult<T>::await_suspend(std::coroutine_handle<> handle)
    {
        auto co = std::coroutine_handle<PromiseTypeBase>::from_address(handle.address()).promise().getCoroutine();
        m_coroutine = co;
        if(co.expired()) {
            LogError("AsyncResult Coroutine expired");
            return false;
        }
        auto co_ptr = co.lock();
        co_ptr->modToSuspend();
        if(m_event->onSuspend(Waker(co))) {
            return true;
        }
        co_ptr->modToRunning();
        return false;  // NOT suspending, continue immediately
    }

    template<CoType T>
    inline T AsyncResult<T>::await_resume() const
    {
        if(m_coroutine.expired()) {
            return this->m_event->onResume();
        }
        return this->m_event->onResume();
    }


}

#endif
