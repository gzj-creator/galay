#include "TimeEvent.h"
#include "galay/utils/System.h"


namespace galay::details
{
    SleepforEvent::SleepforEvent(TimerManager* manager, std::chrono::milliseconds ms)
        : TimeEvent<nil>(manager), m_ms(ms)
    {
    }

    bool SleepforEvent::ready()
    {
        return false;
    }

    bool SleepforEvent::suspend(Waker waker) 
    {
        Timer::ptr timer = std::make_shared<Timer>(m_ms, [waker]() mutable{
            waker.wakeUp();
        });
        m_manager->push(timer);
        return TimeEvent<nil>::suspend(waker);
    }
}