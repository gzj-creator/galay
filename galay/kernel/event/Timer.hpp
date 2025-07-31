#ifndef GALAY_TIMER_HPP
#define GALAY_TIMER_HPP

#include <any>
#include <memory>
#include <atomic>
#include <string>
#include <queue>
#include <set>
#include <list>
#include <functional>
#include <shared_mutex>
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include "galay/common/Base.h"
#include "Event.h"
#include "galay/utils/System.h"

namespace galay::details {
class TimeEvent;
class TimerManager;
}

namespace galay 
{

/*
    if timer is cancled and callback is not executed, Success while return false
*/
class Timer: public std::enable_shared_from_this<Timer> 
{
    friend class details::TimeEvent;
public:
    using ptr = std::shared_ptr<Timer>;
    using wptr = std::weak_ptr<Timer>;


    using timer_callback_t = std::function<void(std::weak_ptr<details::TimeEvent>, Timer::ptr)>;
    using timer_manager_ptr = std::weak_ptr<details::TimerManager>;

    static Timer::ptr create(timer_callback_t &&func);

    Timer(timer_callback_t &&func);
    int64_t getDeadline() const;
    int64_t getTimeout() const;
    int64_t getRemainTime() const;
    std::any& getUserData() { return m_context; };
    //取消状态的Timer才能调用
    bool resetTimeout(int64_t timeout);


    void execute(std::weak_ptr<details::TimeEvent> event);
    void setisCancel(bool cancel);
    bool isCancel() const;

    void setTimerManager(timer_manager_ptr manager);
    timer_manager_ptr getTimerManager() const;
private:
    std::any m_context;
    std::atomic_int64_t m_deadline{ -1 };
    std::atomic_bool m_cancel {true};
    timer_callback_t m_callback;
    timer_manager_ptr m_manager;
};

}

namespace galay::details 
{
    

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#define DEFAULT_TIMEEVENT_ID_CAPACITY 1024

class TimeEventIDStore
{
public:
    using ptr = std::shared_ptr<TimeEventIDStore>;
    //[0, capacity)
    explicit TimeEventIDStore(int capacity);
    bool getEventId(int& id);
    bool recycleEventId(int id);
private:
    int *m_temp;
    int m_capacity;
    moodycamel::ConcurrentQueue<int> m_eventIds;
};
#endif


class TimerManager
{
public:
    using ptr = std::shared_ptr<TimerManager>;
    using wptr = std::weak_ptr<TimerManager>;

    virtual std::list<Timer::ptr> GetArrivedTimers() = 0;
    virtual TimerManagerType type() = 0;
    virtual Timer::ptr top() = 0;
    virtual bool isEmpty()= 0;
    virtual size_t size() = 0;
    virtual void push(Timer::ptr timer) = 0;
    virtual void updateTimers(void* ctx) = 0;
    virtual int64_t onceLoopTimeout() = 0;
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

    std::list<Timer::ptr> GetArrivedTimers() override;
    TimerManagerType type() override;
    Timer::ptr top() override;
    bool isEmpty() override;
    size_t size() override { std::shared_lock lock(m_mutex); return m_timers.size(); }
    void push(Timer::ptr timer) override;
    void updateTimers(void* ctx) override;
    int64_t onceLoopTimeout() override;
private:
    std::shared_mutex m_mutex;
    std::priority_queue<Timer::ptr, std::vector<std::shared_ptr<Timer>>, TimerCompare> m_timers;
};

class RbtreeTimerManager: public TimerManager
{
public:
    using ptr = std::shared_ptr<RbtreeTimerManager>;
    class TimerCompare
    {
    public:
        TimerCompare() = default;
        bool operator()(const Timer::ptr &a, const Timer::ptr &b) const;
    };

    std::list<Timer::ptr> GetArrivedTimers() override;
    TimerManagerType type() override;
    Timer::ptr top() override;
    size_t size() override { std::shared_lock lock(m_mutex); return m_timers.size(); }
    bool isEmpty() override;
    void push(Timer::ptr timer) override;
    void updateTimers(void* ctx) override;
    int64_t onceLoopTimeout() override;
private:
    std::shared_mutex m_mutex;
    std::set<Timer::ptr, TimerCompare> m_timers;
};

class TimeEvent: public Event, public std::enable_shared_from_this<TimeEvent>
{
protected:
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    static TimeEventIDStore g_idStore; 
#endif

    friend class PriorityQueueTimerManager;
    friend class RbtreeTimerManager;
public:
    using ptr = std::shared_ptr<TimeEvent>;
    using wptr = std::weak_ptr<TimeEvent>;
    static bool CreateHandle(GHandle& handle);
    
    TimeEvent(GHandle handle, EventScheduler* scheduler, TimerManagerType type);
    std::string name() override { return "TimeEvent"; }
    EventType getEventType() override { return kEventTypeTimer; };

    int64_t onceLoopTimeout();

    void handleEvent() override;
    GHandle getHandle() override { return m_handle; }
    bool setEventScheduler(EventScheduler* engine) override;
    EventScheduler* belongEventScheduler() override;

    void addTimer(const Timer::ptr& timer, int64_t timeout);

    TimerManager::ptr getTimerManager() { return m_timer_manager; };
    
    ~TimeEvent() override;
private:
    void activeTimerManager();
private:
    GHandle m_handle;
    std::atomic<EventScheduler*> m_scheduler;
    TimerManager::ptr m_timer_manager;
};


}

#endif