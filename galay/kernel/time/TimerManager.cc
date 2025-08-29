#include "TimerManager.h"
#include "galay/utils/System.h"
#include "galay/kernel/event/Event.h"
#if defined(USE_EPOLL)
    #include <sys/timerfd.h>
#elif defined(USE_KQUEUE)
    #include <unistd.h>
#endif

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

    PriorityQueueTimerManager::PriorityQueueTimerManager(TimerActivator::ptr activator)
        : TimerManager(activator)
    {
    }

    void PriorityQueueTimerManager::start()
    {
        m_event = std::make_unique<details::InnerTimeEvent>(this);
    }

    void PriorityQueueTimerManager::stop()
    {
        if(m_event.get()) m_activator->deactive(m_event.get());
    }

    std::list<Timer::ptr> PriorityQueueTimerManager::getArrivedTimers()
    {
        std::list<Timer::ptr> timers;
        int64_t now = utils::getCurrentTimeMs();
    #ifdef USE_EPOLL
        uint64_t times = 0;
        read(m_event->getHandle().fd, &times, sizeof(uint64_t));
    #endif
        std::unique_lock lock(this->m_mutex);
        while (!m_timers.empty() && m_timers.top()->getDeadline() <= now) {
            auto timer = m_timers.top();
            m_timers.pop();
            timers.emplace_back(timer);
        }
        if(!m_timers.empty()) m_activator->active(m_timers.top(), m_event.get());
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


    void PriorityQueueTimerManager::push(Timer::ptr timer)
    {
        std::unique_lock lock(this->m_mutex);
        m_timers.push(timer);
        if(m_timers.top().get() == timer.get() ) {
            m_activator->active(timer, m_event.get());
        }
    }
}

std::atomic_uint64_t galay::details::InnerTimeEvent::timer_id = 0;

galay::details::InnerTimeEvent::InnerTimeEvent(TimerManager *manager)
    : m_manager(manager)
{
    #if defined(USE_EPOLL)
        m_handle.fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    #elif defined(USE_KQUEUE)
        m_handle.fd = timer_id.fetch_add(1);
    #endif
}

void galay::details::InnerTimeEvent::handleEvent()
{
    auto timers = m_manager->getArrivedTimers();
    for (auto& timer: timers) {
        timer->execute();
    }
}

galay::details::InnerTimeEvent::~InnerTimeEvent()
{
    ::close(m_handle.fd);
}
