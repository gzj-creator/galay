#ifndef GALAY_TIMER_MANAGER_H
#define GALAY_TIMER_MANAGER_H 

#include "TimerActive.h"
#include <list>
#include <mutex>
#include <queue>
#include <shared_mutex>

namespace galay
{

    class TimerManager
    {
    public:
        using ptr = std::shared_ptr<TimerManager>;
        using wptr = std::weak_ptr<TimerManager>;

        virtual std::list<Timer::ptr> getArrivedTimers(TimerActive::ptr active, details::Event* event) = 0;
        virtual Timer::ptr top() = 0;
        virtual bool isEmpty()= 0;
        virtual size_t size() = 0;
        virtual void push(Timer::ptr timer, TimerActive::ptr active, details::Event* event) = 0;
    };

   
    class PriorityQueueTimerManager: public TimerManager
    {
    public:
        using ptr = std::shared_ptr<PriorityQueueTimerManager>;
        class TimerCompare
        {
        public:
            TimerCompare() = default;
            bool operator()(const Timer::ptr &a, const Timer::ptr &b) const;
        };
        std::list<Timer::ptr> getArrivedTimers(TimerActive::ptr active, details::Event* event) override;
        Timer::ptr top() override;
        bool isEmpty() override;
        size_t size() override { std::shared_lock lock(m_mutex); return m_timers.size(); }
        void push(Timer::ptr timer, TimerActive::ptr active, details::Event* event) override;
    private:
        std::shared_mutex m_mutex;
        std::priority_queue<Timer::ptr, std::vector<std::shared_ptr<Timer>>, TimerCompare> m_timers;
    };
}



#endif