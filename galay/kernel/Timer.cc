#include "Timer.h"
#include "galay/utils/System.h"
#include <functional>

namespace galay 
{ 
    Timer::Timer(std::chrono::milliseconds ms, const std::function<void()>& callback)
        : m_cancel(false),m_deadline(std::chrono::time_point_cast<std::chrono::milliseconds>(\
            std::chrono::system_clock::now()).time_since_epoch().count() + ms.count()),\
            m_callback(callback)
            
    {
    }

    int64_t Timer::getDeadline()
    {
        beforeAction();
        return m_deadline;
    }

    int64_t Timer::getRemainTime()
    {
        beforeAction();
        int64_t now = utils::getCurrentTimeMs();
        const int64_t time = m_deadline - now;
        return time < 0 ? 0 : time;
    }

    void Timer::beforeAction()
    {
        if(m_expect_deadline > m_deadline) {
            m_deadline = m_expect_deadline;
            m_expect_deadline = -1;
        } else {
            m_expect_deadline = -1;
        }
    }

    void Timer::delay(std::chrono::milliseconds ms)
    {
        m_expect_deadline = std::chrono::time_point_cast<std::chrono::milliseconds>(\
            std::chrono::system_clock::now()).time_since_epoch().count() + ms.count();
    }

    void Timer::execute()
    {
        if(m_cancel) {
            return;
        }
        m_callback();
    }

    void Timer::cancel()
    {
       m_cancel = true;
    }


    bool PriorityQueueTimerManager::TimerCompare::operator()(const Timer::ptr& a, const Timer::ptr& b) const
    {
        if (a->getDeadline() > b->getDeadline())
        {
            return true;
        }
        return false;
    }

    void PriorityQueueTimerManager::push(Timer::ptr timer)
    {
        m_timers.emplace(timer);
    }
    
    void PriorityQueueTimerManager::onTimerTick()
    {
        do {
            if(m_timers.empty()) break;
            auto timer = m_timers.top();
            if(timer->getRemainTime() > 0) break;
            timer->execute();
        } while(true);
    }
    
    Timer::ptr PriorityQueueTimerManager::getEarliestTimer() 
    {
        if(m_timers.empty()) return nullptr;
        return m_timers.top();
    }
    
}