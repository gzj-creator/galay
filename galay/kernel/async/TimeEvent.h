#ifndef GALAY_TIME_EVENT_H
#define GALAY_TIME_EVENT_H 

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/kernel/time/TimerManager.h"
#include "galay/common/Common.h"

namespace galay::details
{

    struct TimeStatusContext {
        GHandle m_handle;
        EventScheduler* m_scheduler;
        TimerActive::ptr m_active = nullptr;
        TimerManager::ptr m_manager = nullptr;
    };
    
    template<CoType T>
    class TimeEvent: public AsyncEvent<T>
    {
    public:
        using ptr = std::shared_ptr<TimeEvent>;
        using wptr = std::weak_ptr<TimeEvent>;

        TimeEvent(TimeStatusContext& context) 
            :m_context(context) {};
        std::string name() override { return "TimeEvent"; }
        EventType getEventType() const override { return kEventTypeNone; };
        GHandle& getHandle() override { return m_context.m_handle; }
        void handleEvent() override {}
        bool suspend(Waker waker) override
        {
            this->m_waker = waker;
            return true;
        }
    protected:
        TimeStatusContext& m_context;
    };

    class CloseTimeEvent: public TimeEvent<ValueWrapper<bool>> {
    public:
        CloseTimeEvent(TimeStatusContext& context)
            : TimeEvent(context) {}
        EventType getEventType() const override { return EventType::kEventTypeNone; }
        std::string name() override { return "CloseTimeEvent"; }
        void handleEvent() override {}

        bool ready() override;
        bool suspend(Waker waker) override;
    };

    // template <CoType T>
    // class TimeoutEvent: public TimeEvent<T> {
    // public:
    //     TimeoutEvent(TimeStatusContext& context, const std::function<AsyncResult<T>()>& func, std::chrono::milliseconds ms)
    //         : TimeEvent<T>(context), m_ms(ms), m_func(func) {}
    //     EventType getEventType() const override { return EventType::kEventTypeTimer; }
    //     std::string name() override { return "TimeEvent"; }
    //     void handleEvent() override {

    //     }

    //     bool ready() override;
    //     bool suspend(Waker waker) override;
    // protected:
    //     std::chrono::milliseconds m_ms;
    //     std::function<AsyncResult<T>()> m_func;  
    // };

}


#endif