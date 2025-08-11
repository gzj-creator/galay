#include "TimerManager.h"
#include "galay/utils/System.h"

namespace galay
{
    bool PriorityQueueTimerManager::TimerCompare::operator()(const Timer::ptr& a, const Timer::ptr& b) const
    {
        if (a->getDeadline() > b->getDeadline())
        {
            return true;
        }
        return false;
    }

    std::list<Timer::ptr> PriorityQueueTimerManager::getArrivedTimers(TimerActive::ptr active, details::Event* event)
    {
        std::list<Timer::ptr> timers;
        int64_t now = utils::getCurrentTimeMs();
        std::unique_lock lock(this->m_mutex);
        while (!m_timers.empty() && m_timers.top()->getDeadline() <= now) {
            auto timer = m_timers.top();
            m_timers.pop();
            timers.emplace_back(timer);
        }
        active->active(m_timers.top(), event);
        return timers;
    }


    Timer::ptr PriorityQueueTimerManager::top()
    {
        std::shared_lock lock(this->m_mutex);
        if (m_timers.empty())
        {
            return nullptr;
        }
        return m_timers.top();
    }


    bool PriorityQueueTimerManager::isEmpty()
    {
        std::shared_lock lock(m_mutex);
        return m_timers.empty();
    }


    void PriorityQueueTimerManager::push(Timer::ptr timer, TimerActive::ptr active, details::Event* event)
    {
        std::unique_lock lock(this->m_mutex);
        m_timers.push(timer);
        if(m_timers.top().get() == timer.get() ) {
            active->active(timer, event);
        }
    }
}