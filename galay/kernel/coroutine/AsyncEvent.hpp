#ifndef GALAY_ASYNC_EVENT_HPP
#define GALAY_ASYNC_EVENT_HPP

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

}



#endif