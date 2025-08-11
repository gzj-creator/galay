#ifndef GALAY_COROUTINE_RESULT_HPP
#define GALAY_COROUTINE_RESULT_HPP

#include "Waker.h"
#include "galay/kernel/event/Event.h"

namespace galay
{

    template <CoType T>
    class AsyncEvent: public details::Event
    {
    public:

        using ptr = std::shared_ptr<AsyncEvent>;
        using wptr = std::weak_ptr<AsyncEvent>;

        explicit AsyncEvent();
        explicit AsyncEvent(T&& result);

        //return true while not suspend
        virtual bool ready() = 0;
        //return true while suspend
        virtual bool suspend(Waker waker) = 0;

        virtual T resume();
        ~AsyncEvent() override = default;
    protected:
        T m_result;
        Waker m_waker;
    };

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
        AsyncResult(T&& result);
        bool await_ready();
        //true will suspend, false will not
        bool await_suspend(std::coroutine_handle<> handle);
        T await_resume() const;
    protected:
        CoroutineBase::wptr m_coroutine;
        typename AsyncEvent<T>::ptr m_event;
    };

}


#include "Result.inl"

#endif