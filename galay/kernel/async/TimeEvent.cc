#include "TimeEvent.h"
#include "galay/utils/System.h"


namespace galay::details
{
    SleepforEvent::SleepforEvent(TimerManager* manager, std::chrono::milliseconds ms)
        : TimeEvent<nil>(manager), m_ms(ms)
    {
    }

    bool SleepforEvent::onReady()
    { 
        return m_ms == std::chrono::milliseconds::zero();
    }

    bool SleepforEvent::onSuspend(Waker waker) 
    {
        auto timer = std::make_shared<Timer>(m_ms, waker);
        m_manager->push(timer);
        return TimeEvent<nil>::onSuspend(waker);
    }

    TimeWaitEvent::TimeWaitEvent(TimerManager *manager, std::chrono::milliseconds ms, const std::function<void()>& callback)
        : TimeEvent<nil>(manager), m_ms(ms), m_callback(callback)
    {
    }

    bool TimeWaitEvent::onReady()
    {
        return m_ms == std::chrono::milliseconds::zero();
    }

    bool TimeWaitEvent::onSuspend(Waker waker)
    {
        auto timer = std::make_shared<Timer>(m_ms, waker);
        m_manager->push(timer);
        return TimeEvent<nil>::onSuspend(waker);
    }

    nil TimeWaitEvent::onResume()
    {
        m_callback();
        return TimeEvent<nil>::onResume();
    }
}