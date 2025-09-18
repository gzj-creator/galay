#ifndef GALAY_TIME_EVENT_H
#define GALAY_TIME_EVENT_H 

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/kernel/time/TimerManager.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "galay/common/Common.h"

namespace galay::details
{
    
    template<CoType T>
    class TimeEvent: public AsyncEvent<T>, public Event
    {
    public:
        using ptr = std::shared_ptr<TimeEvent>;
        using wptr = std::weak_ptr<TimeEvent>;

        TimeEvent(TimerManager* manager) 
            :m_manager(manager) {} 
        std::string name() override { return "TimeEvent"; }
        EventType getEventType() const override { return kEventTypeNone; };
        GHandle getHandle() override { return {}; }
        void handleEvent() override {}
        bool onSuspend(Waker waker) override
        {
            this->m_waker = waker;
            return true;
        }
    protected:
        TimerManager* m_manager;
    };

    class SleepforEvent: public TimeEvent<nil>
    {
    public:
        SleepforEvent(TimerManager* manager, std::chrono::milliseconds ms);
        std::string name() override { return "SleepforEvent"; }
        void handleEvent() override {}

        bool onReady() override;
        bool onSuspend(Waker waker) override;
    private:
        std::chrono::milliseconds m_ms;
    };

    class TimeWaitEvent: public TimeEvent<nil>
    {
    public:
        TimeWaitEvent(TimerManager* manager, std::chrono::milliseconds ms, const std::function<void()>& callback);
        std::string name() override { return "TimeWaitEvent"; }
        void handleEvent() override {}
        bool onReady() override;
        bool onSuspend(Waker waker) override;
        nil onResume() override;
    private:
        std::chrono::milliseconds m_ms;
        std::function<void()> m_callback;
    };
}


#endif