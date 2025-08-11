#ifndef GALAY_TIMER_ACTIVE_H
#define GALAY_TIMER_ACTIVE_H 

#include "Timer.h"
#include "galay/common/Base.h"
#include "galay/kernel/event/EventScheduler.h"


namespace galay
{
    class TimerActive
    {
    public:
        using ptr = std::shared_ptr<TimerActive>;
        virtual void active(Timer::ptr timer, details::Event* event) = 0;
    };

#if defined(USE_EPOLL)
    class EpollTimerActive: public TimerActive
    {
    public:
        EpollTimerActive(EventScheduler* scheduler)
            :m_scheduler(scheduler)
        {
        }

        void active(Timer::ptr timer, details::Event* event) override;

    private:
        EventScheduler* m_scheduler;
    };
#elif defined(USE_KQUEUE)
    class KQueueTimerActive: public TimerActive
    {
    public:
        KQueueTimerActive(EventScheduler* scheduler)
            :m_scheduler(scheduler)
        {
        }

        void active(Timer::ptr timer, details::Event* event) override;

    private:
        EventScheduler* m_scheduler;
    };
#elif defined(USE_IOURING)
#else
#endif

}


#endif