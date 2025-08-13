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
        return m_event->ready();
    }

    template<CoType T>
    inline bool AsyncResult<T>::await_suspend(std::coroutine_handle<> handle)
    {
        auto co = std::coroutine_handle<PromiseTypeBase>::from_address(handle.address()).promise().getCoroutine();
        m_coroutine = co;
        if(co.expired()) {
            throw std::runtime_error("Coroutine expired");
        }
        if(m_event->suspend(Waker(co))) {
            while(!co.lock()->become(CoroutineStatus::Suspended)) {
                throw std::runtime_error("Coroutine become Failed");
            } 
            return true;
        }
        return false;
    }

    template<CoType T>
    inline T AsyncResult<T>::await_resume() const
    {
        if(m_coroutine.expired()) {
            return this->m_event->resume();
        }
        while(!m_coroutine.lock()->become(CoroutineStatus::Running)) {
            throw std::runtime_error("Coroutine become Failed");
        }
        return this->m_event->resume();
    }


}

#endif
