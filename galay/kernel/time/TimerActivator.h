#ifndef GALAY_TIMER_ACTIVATOR_H
#define GALAY_TIMER_ACTIVATOR_H 

#include "Timer.h"
#include "galay/common/Base.h"
#include "galay/kernel/event/EventScheduler.h"


namespace galay
{
    class TimerActivator
    {
    public:
        using ptr = std::shared_ptr<TimerActivator>;
        virtual void active(Timer::ptr timer, details::Event* event) = 0;
        virtual void deactive(details::Event* event) = 0;
        virtual ~TimerActivator() = default;
    };

#if defined(USE_EPOLL)
    class EpollTimerActive: public TimerActivator
    {
    public:
        EpollTimerActive(EventScheduler* scheduler)
            :m_scheduler(scheduler)
        {
        }

        void active(Timer::ptr timer, details::Event* event) override;
        void deactive(details::Event* event) override;
    private:
        EventScheduler* m_scheduler;
    };
#elif defined(USE_KQUEUE)
    class KQueueTimerActive: public TimerActivator
    {
    public:
        KQueueTimerActive(EventScheduler* scheduler)
            :m_scheduler(scheduler)
        {
        }
        void active(Timer::ptr timer, details::Event* event) override;
        void deactive(details::Event* event) override;
    private:
        EventScheduler* m_scheduler;
    };
#elif defined(USE_IOURING)
    class IOUringTimerActive: public TimerActivator
    {
    public:
        IOUringTimerActive(EventScheduler* scheduler)
            :m_scheduler(scheduler)
        {
        }
        void active(Timer::ptr timer, details::Event* event) override;
        void deactive(details::Event* event) override;
    private:
        EventScheduler* m_scheduler;
    };
#else
#endif

}


#endif