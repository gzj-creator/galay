#ifndef GALAY_ASYNC_EVENT_HPP
#define GALAY_ASYNC_EVENT_HPP

#include "Waker.h"
#include "galay/kernel/event/Event.h"

namespace galay
{

    template <CoType T>
    class AsyncEvent
    {
    public:

        using ptr = std::shared_ptr<AsyncEvent>;
        using wptr = std::weak_ptr<AsyncEvent>;

        explicit AsyncEvent();
        explicit AsyncEvent(T&& result);

        //return true while not suspend
        virtual bool onReady() = 0;
        //return true while suspend
        virtual bool onSuspend(Waker waker) = 0;

        virtual T onResume();
        virtual ~AsyncEvent() = default;
    protected:
        T m_result;
        Waker m_waker;
    };

    template <CoType T>
    class ResultEvent final : public AsyncEvent<T> {
    public:
        explicit ResultEvent(T&& result) : AsyncEvent<T>(std::move(result)) {}
        bool onReady() override { return true; }
        bool onSuspend([[maybe_unused]] Waker waker) override { return false; }
        T onResume() override { return std::move(this->m_result); }
    private:
        GHandle m_handle;
    };

    template <CoType T>
    inline AsyncEvent<T>::AsyncEvent()
        : m_result()
    {
    }

    template <CoType T>
    inline AsyncEvent<T>::AsyncEvent(T &&result)
        : m_result(std::move(result))
    {
    }

    template <CoType T>
    T AsyncEvent<T>::onResume()
    {
        return std::move(m_result);
    }

}

#endif