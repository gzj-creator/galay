#ifndef GALAY_TIMER_MANAGER_H
#define GALAY_TIMER_MANAGER_H 

#include "TimerActivator.h"
#include <list>
#include <mutex>
#include <queue>
#include <shared_mutex>

namespace galay::details  {
    class InnerTimeEvent;
}

namespace galay
{

    class TimerManager
    {
    public:
        using ptr = std::shared_ptr<TimerManager>;
        using wptr = std::weak_ptr<TimerManager>;
        TimerManager(TimerActivator::ptr activator)
            : m_activator(activator) {}
        virtual void start() = 0;
        virtual void stop() = 0;
        virtual std::list<Timer::ptr> getArrivedTimers() = 0;
        virtual Timer::ptr top() = 0;
        virtual bool isEmpty()= 0;
        virtual size_t size() = 0;
        virtual void push(Timer::ptr timer) = 0;
        virtual ~TimerManager() = default;
    protected:
        TimerActivator::ptr m_activator;
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
        PriorityQueueTimerManager(TimerActivator::ptr activator);
        void start() override;
        void stop() override;
        std::list<Timer::ptr> getArrivedTimers() override;
        Timer::ptr top() override;
        bool isEmpty() override;
        size_t size() override { std::shared_lock lock(m_mutex); return m_timers.size(); }
        void push(Timer::ptr timer) override;
        ~PriorityQueueTimerManager();
    private:
        std::shared_mutex m_mutex;
        std::unique_ptr<details::InnerTimeEvent> m_event;
        std::priority_queue<Timer::ptr, std::vector<std::shared_ptr<Timer>>, TimerCompare> m_timers;
    };
}

namespace galay::details 
{
    class InnerTimeEvent: public Event 
    {
        static std::atomic_uint64_t timer_id;
    public: 
        InnerTimeEvent(TimerManager* manager);
        std::string name() override { return "InnerTimeEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return kEventTypeTimer; }
        GHandle getHandle() override { return m_handle; }
        ~InnerTimeEvent() override;
    private:
        GHandle m_handle;
        TimerManager* m_manager;
    };
}

#endif