#ifndef GALAY_ASYNC_EVENT_HPP
#define GALAY_ASYNC_EVENT_HPP

#include "Waker.h"
#include "galay/kernel/event/Event.h"

namespace galay
{

#define DEFAULT_EVENT_IMPL() \
    void handleEvent() override {} \
    EventType getEventType() const override { return EventType::kEventTypeNone; }\
    GHandle getHandle() override { return m_handle; }

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
    class ResultEvent final : public AsyncEvent<T> {
    public:
        explicit ResultEvent(T&& result) : AsyncEvent<T>(std::move(result)) {}
        bool ready() override { return true; }
        bool suspend([[maybe_unused]] Waker waker) override { return false; }
        T resume() override { return std::move(this->m_result); }
        std::string name() override { return "ResultEvent"; }
        DEFAULT_EVENT_IMPL();
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
    T AsyncEvent<T>::resume()
    {
        return std::move(m_result);
    }

}

#endif