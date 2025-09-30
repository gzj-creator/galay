#ifndef GALAY_TIME_EVENT_H
#define GALAY_TIME_EVENT_H 

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/kernel/time/TimerManager.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "galay/common/Common.h"

namespace galay::details
{
    
    template<CoType T>
    class TimeEvent: public AsyncEvent<T>
    {
    public:
        using ptr = std::shared_ptr<TimeEvent>;
        using wptr = std::weak_ptr<TimeEvent>;

        TimeEvent(TimerManager* manager) 
            :m_manager(manager) {} 
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
        bool onReady() override;
        bool onSuspend(Waker waker) override;
    private:
        std::chrono::milliseconds m_ms;
    };

    class TimeWaitEvent: public TimeEvent<nil>
    {
    public:
        TimeWaitEvent(TimerManager* manager, std::chrono::milliseconds ms, const std::function<void()>& callback);
        bool onReady() override;
        bool onSuspend(Waker waker) override;
        nil onResume() override;
    private:
        std::chrono::milliseconds m_ms;
        std::function<void()> m_callback;
    };
}


#endif