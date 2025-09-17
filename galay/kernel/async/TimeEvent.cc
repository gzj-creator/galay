#include "TimeEvent.h"
#include "galay/utils/System.h"


namespace galay::details
{
    SleepforEvent::SleepforEvent(TimerManager* manager, Timer::ptr timer, std::chrono::milliseconds ms)
        : TimeEvent<nil>(manager), m_timer(timer), m_ms(ms)
    {
    }

    bool SleepforEvent::onReady()
    { 
        return m_ms == std::chrono::milliseconds::zero();
    }

    bool SleepforEvent::onSuspend(Waker waker) 
    {
        m_timer->setFunction([waker]() mutable{
            waker.wakeUp();
        });
        m_timer->reset(m_ms);
        m_manager->push(m_timer);
        return TimeEvent<nil>::onSuspend(waker);
    }
}