#ifndef GALAY_RESULT_INL
#define GALAY_RESULT_INL


#include "Result.hpp"


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
        if(m_event->onSuspend(Waker(co))) {
            // onSuspend returned true: coroutine WILL be suspended
            while(!co.lock()->become(CoroutineStatus::Suspended)) {
                LogError("AsyncResult Coroutine become suspend error");
            }
            return true;  // Suspending
        }
        // onSuspend returned false: coroutine will NOT be suspended
        // Need to mark it as running now, since we won't suspend
        while(!co.lock()->become(CoroutineStatus::Running)) {
            LogError("AsyncResult Coroutine become running error (in await_suspend)");
        }
        return false;  // NOT suspending, continue immediately
    }

    template<CoType T>
    inline T AsyncResult<T>::await_resume() const
    {
        if(m_coroutine.expired()) {
            return this->m_event->onResume();
        }
        // Try to transition to Running status if not already
        auto co_ptr = m_coroutine.lock();
        if(co_ptr) {
            // Only if we can transition to running
            while(!co_ptr->become(CoroutineStatus::Running)) {
                LogError("AsyncResult Coroutine become running error");
            }
        }
        return this->m_event->onResume();
    }


}

#endif
