#include "Timer.hpp"
#include "EventEngine.h"
#if defined(__linux__)
#include <sys/timerfd.h>
#include <unistd.h>
#elif  defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)

#endif



namespace galay
{

    Timer::ptr Timer::create(timer_callback_t &&func)
    {
        return std::make_shared<Timer>(std::move(func));
    }

    Timer::Timer(timer_callback_t &&func)
    {
        m_callback = std::move(func);
    }


    int64_t
    Timer::getDeadline() const
    {
        return m_deadline;
    }

    int64_t Timer::getTimeout() const
    {
        int64_t now = utils::getCurrentTimeMs();
        return m_deadline - now;
    }

    int64_t
    Timer::getRemainTime() const
    {
        int64_t now = utils::getCurrentTimeMs();
        const int64_t time = m_deadline - now;
        return time < 0 ? 0 : time;
    }

    bool
    Timer::resetTimeout(int64_t timeout)
    {
        if (!isCancel()) return false;
        int64_t old = m_deadline.load();
        int64_t now = utils::getCurrentTimeMs();
        if(!m_deadline.compare_exchange_strong(old, now + timeout))
        {
            return false;
        }
        int64_t time = m_deadline.load();
        return true;
    }

    void
    Timer::execute(std::weak_ptr<details::TimeEvent> event)
    {
        if (m_cancel.load())
            return;
        m_callback(event, shared_from_this());
    }

    void
    Timer::setisCancel(bool cancel)
    {
        m_cancel.store(cancel);
    }

    bool Timer::isCancel() const
    {
        return m_cancel.load();
    }

    void Timer::setTimerManager(timer_manager_ptr manager)
    {
        m_manager = manager;
    }

    Timer::timer_manager_ptr Timer::getTimerManager() const
    {
        return m_manager;
    }

}


namespace galay::details
{

    #if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    TimeEventIDStore::TimeEventIDStore(const int capacity)
    {
        m_temp = static_cast<int*>(calloc(capacity, sizeof(int)));
        for(int i = 0; i < capacity; i++){
            m_temp[i] = i;
        }
        m_capacity = capacity;
        m_eventIds.enqueue_bulk(m_temp, capacity);
        free(m_temp);
    }

    bool TimeEventIDStore::getEventId(int& id)
    {
        return m_eventIds.try_dequeue(id);
    }

    bool TimeEventIDStore::recycleEventId(const int id)
    {
        return m_eventIds.enqueue(id);
    }


    TimeEventIDStore TimeEvent::g_idStore(DEFAULT_TIMEEVENT_ID_CAPACITY);

    bool TimeEvent::CreateHandle(GHandle &handle)
    {
        return g_idStore.getEventId(handle.fd);
    }


    #elif defined(__linux__)

    bool TimeEvent::CreateHandle(GHandle& handle)
    {
        handle.fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        return handle.fd != -1;
    }

    #endif


    TimeEvent::TimeEvent(const GHandle handle, EventScheduler* scheduler, TimerManagerType type)
        : m_handle(handle), m_scheduler(scheduler)
    {
        switch (type)
        {
        case TimerManagerType::kTimerManagerTypePriorityQueue :
            m_timer_manager = std::make_shared<PriorityQueueTimerManager>();
            break;
        case TimerManagerType::kTimerManagerTypeRbTree :
            m_timer_manager = std::make_shared<RbtreeTimerManager>();
            break;
        case TimerManagerType::kTimerManagerTypeTimeWheel :
            /*To Do*/
            break;
        default:
            break;
        }
    #if defined(__linux__)
        scheduler->addEvent(this, nullptr);
    #endif
    }

    int64_t TimeEvent::onceLoopTimeout()
    {
        return m_timer_manager->onceLoopTimeout();
    }

    void TimeEvent::handleEvent()
    {
#ifdef __linux__
        m_scheduler.load()->modEvent(this, nullptr);
#endif
        std::list<Timer::ptr> timers = m_timer_manager->GetArrivedTimers();
        for (auto timer: timers)
        {
            timer->execute(weak_from_this());
        }
        activeTimerManager();
    }

    bool TimeEvent::setEventScheduler(EventScheduler *scheduler)
    {
        auto t = m_scheduler.load();
        if(!m_scheduler.compare_exchange_strong(t, scheduler)) {
            return false;
        }
        return true;
    }

    EventScheduler* TimeEvent::belongEventScheduler()
    {
        return m_scheduler.load();
    }

    void TimeEvent::addTimer(const Timer::ptr &timer, const int64_t timeout)
    {
        timer->resetTimeout(timeout);
        timer->setisCancel(false);
        timer->setTimerManager(m_timer_manager);
        this->m_timer_manager->push(timer);
        activeTimerManager();
    }

    TimeEvent::~TimeEvent()
    {
        m_scheduler.load()->delEvent(this, nullptr);
    #if defined(__linux__)
        close(m_handle.fd);
    #elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        g_idStore.recycleEventId(m_handle.fd);
    #endif
    }

    void TimeEvent::activeTimerManager()
    {
        switch (m_timer_manager->type())
        {
        case TimerManagerType::kTimerManagerTypePriorityQueue :
            this->m_timer_manager->updateTimers(this);
            break;
        case TimerManagerType::kTimerManagerTypeRbTree :
            this->m_timer_manager->updateTimers(this);
            break;
        case TimerManagerType::kTimerManagerTypeTimeWheel :
            /*To Do*/
            break;
        default:
            break;
        }
    }

    bool PriorityQueueTimerManager::TimerCompare::operator()(const Timer::ptr& a, const Timer::ptr& b) const
    {
        if (a->getDeadline() > b->getDeadline())
        {
            return true;
        }
        return false;
    }


    std::list<Timer::ptr> PriorityQueueTimerManager::GetArrivedTimers()
    {
        std::list<Timer::ptr> timers;
        int64_t now = utils::getCurrentTimeMs();
        std::unique_lock lock(this->m_mutex);
        while (!m_timers.empty() && m_timers.top()->getDeadline() <= now) {
            auto timer = m_timers.top();
            m_timers.pop();
            timers.emplace_back(timer);
        }
        return timers;
    }

    TimerManagerType PriorityQueueTimerManager::type()
    {
        return TimerManagerType::kTimerManagerTypePriorityQueue;
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


    void PriorityQueueTimerManager::push(Timer::ptr timer)
    {
        std::unique_lock lock(this->m_mutex);
        m_timers.push(timer);
    }

    void PriorityQueueTimerManager::updateTimers(void* ctx)
    {
    #ifdef __linux__
        TimeEvent* event = static_cast<TimeEvent*>(ctx);
        struct timespec abstime;
        if (isEmpty())
        {
            abstime.tv_sec = 0;
            abstime.tv_nsec = 0;
        }
        else
        {
            auto timer = top();
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
        timerfd_settime(event->m_handle.fd, 0, &its, nullptr);
    #elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        TimeEvent* event = static_cast<TimeEvent*>(ctx);
        if(isEmpty()) {
            return;
        } else {
            auto timer = top();
            uint64_t timeout = timer->getTimeout();
            event->m_scheduler.load()->modEvent(event, &timeout);
        }
    #endif
    }

    int64_t PriorityQueueTimerManager::onceLoopTimeout()
    {
        return -1;
    }

    bool RbtreeTimerManager::TimerCompare::operator()(const Timer::ptr &a, const Timer::ptr &b) const
    {
        if (a->getDeadline() > b->getDeadline())
        {
            return false;
        }
        return true;
    }


    std::list<Timer::ptr> RbtreeTimerManager::GetArrivedTimers()
    {
        std::list<Timer::ptr> timers;
        int64_t now = utils::getCurrentTimeMs();
        std::unique_lock lock(this->m_mutex);
        while (!m_timers.empty() && (*m_timers.begin())->getDeadline() <= now) {
            auto timer = *(m_timers.begin());
            m_timers.erase(m_timers.begin());
            timers.emplace_back(timer);
        }
        return timers;
    }


    TimerManagerType RbtreeTimerManager::type()
    {
        return TimerManagerType::kTimerManagerTypeRbTree;
    }

    Timer::ptr RbtreeTimerManager::top()
    {
        std::shared_lock lock(m_mutex);
        if(m_timers.empty()) {
            return nullptr;
        }
        return *m_timers.begin();
    }

    bool RbtreeTimerManager::isEmpty()
    {
        std::shared_lock lock(m_mutex);
        return m_timers.empty();
    }

    void RbtreeTimerManager::push(Timer::ptr timer)
    {
        std::unique_lock lock(m_mutex);
        m_timers.insert(timer);
    }

    void RbtreeTimerManager::updateTimers(void *ctx)
    {
    #ifdef __linux__
        TimeEvent* event = static_cast<TimeEvent*>(ctx);
        struct timespec abstime;
        if (isEmpty())
        {
            abstime.tv_sec = 0;
            abstime.tv_nsec = 0;
        }
        else
        {
            auto timer = top();
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
        timerfd_settime(event->m_handle.fd, 0, &its, nullptr);
    #elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        TimeEvent* event = static_cast<TimeEvent*>(ctx);
        if(isEmpty()) {
            return;
        } else {
            auto timer = top();
            event->m_scheduler.load()->modEvent(event, timer.get());
        }
    #endif
    }



    int64_t RbtreeTimerManager::onceLoopTimeout()
    {
        return -1;
    }


}