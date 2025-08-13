#ifndef GALAY_COROUTINE_RESULT_HPP
#define GALAY_COROUTINE_RESULT_HPP

#include "AsyncEvent.hpp"

namespace galay
{
    template <CoType T>
    class AsyncResult
    {
    public:
        AsyncResult(const AsyncResult&) = delete;
        AsyncResult(AsyncResult&& other)  noexcept {
            m_event = other.m_event;
            other.m_event = nullptr;
        }

        AsyncResult(typename AsyncEvent<T>::ptr event);
        AsyncResult(T&& value);
        bool await_ready();
        //true will suspend, false will not
        bool await_suspend(std::coroutine_handle<> handle);
        T await_resume() const;
    protected:
        CoroutineBase::wptr m_coroutine;
        typename AsyncEvent<T>::ptr m_event = nullptr;
    };

}


#include "Result.inl"

#endif