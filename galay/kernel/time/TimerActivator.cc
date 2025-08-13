#include "TimerActivator.h"
#include "galay/kernel/event/Event.h"
#if defined(USE_EPOLL)
    #include <sys/timerfd.h>
#endif
namespace galay
{

#if defined(USE_EPOLL)
    void galay::EpollTimerActive::active(Timer::ptr timer, details::Event* event)
    {
        struct timespec abstime;
        if (!timer)
        {
            abstime.tv_sec = 0;
            abstime.tv_nsec = 0;
        }
        else
        {
            int64_t time = timer->getRemainTime();
            if (time != 0)
            {
                abstime.tv_sec = time / 1000;
                abstime.tv_nsec = (time % 1000) * 1000000;
            }
            else
            {
                abstime.tv_sec = 0;
                abstime.tv_nsec = 1;
            }
        }
        struct itimerspec its = {
            .it_interval = {},
            .it_value = abstime};
        if(timerfd_settime(event->getHandle().fd, 0, &its, nullptr)) {
            throw std::runtime_error("timerfd_settime error");
        }
        if(!m_scheduler->activeEvent(event, nullptr)) {
            throw std::runtime_error("modEvent failed");
        }
    }

    void EpollTimerActive::deactive(details::Event *event)
    {
        m_scheduler->removeEvent(event, nullptr);
    }

#elif defined(USE_KQUEUE)
    void galay::KQueueTimerActive::active(Timer::ptr timer)
    {
        uint64_t timeout = timer->getRemainTime();
        m_scheduler->activeEvent(event, &timeout);
    }

#endif
}