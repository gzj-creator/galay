#include "EventScheduler.h"
#include "EventEngine.h"
#include "galay/common/Log.h"
#include "Timer.hpp"

namespace galay{ 

    EventScheduler::EventScheduler()
    {
    #if defined(USE_EPOLL)
        m_engine = std::make_shared<details::EpollEventEngine>();
    #elif defined(USE_IOURING)
        m_engine = std::make_shared<details::IOUringEventEngine>();
    #elif defined(USE_KQUEUE)
        m_engine = std::make_shared<details::KqueueEventEngine>();
    #endif
    }

    EventScheduler::EventScheduler(engine_ptr engine)
        : m_engine(engine)
    {
    }

    bool EventScheduler::addEvent(details::Event* event, void* ctx)
    {
        while(!event->setEventScheduler(this)) {
            LogError("[setEventScheduler failed]");
        }
        return m_engine->addEvent(event, ctx) == 0;
    }


    bool EventScheduler::modEvent(details::Event* event, void* ctx)
    {
        return m_engine->modEvent(event, ctx) == 0;
    }

    bool EventScheduler::delEvent(details::Event* event, void* ctx)
    {
        while(!event->setEventScheduler(nullptr)) {
            LogError("[setEventScheduler failed]");
        }
        return m_engine->delEvent(event, ctx) == 0;
    }

    void EventScheduler::registerOnceLoopCallback(const std::function<void()>& callback)
    {
        m_engine->registerOnceLoopCallback(callback);
    }


    void EventScheduler::makeTimeEvent(TimerManagerType type)
    {
        GHandle handle{};
        details::TimeEvent::CreateHandle(handle);
        m_timer_event = std::make_shared<details::TimeEvent>(handle, this, type);
    }

    std::shared_ptr<details::TimeEvent> EventScheduler::getTimeEvent()
    {
        return m_timer_event;
    }

    bool EventScheduler::start()
    {
        this->m_thread = std::make_unique<std::thread>([this](){
            int timeout = -1;
            if( m_timer_event ) timeout = m_timer_event->onceLoopTimeout();
            m_engine->start(timeout);
            LogTrace("[{}({}) exist successfully]", name(), GetEngine()->getHandle().fd);
        });
        return true;
    }

    bool EventScheduler::stop()
    {
        m_timer_event.reset();
        if(!m_engine->isRunning()) return false;
        m_engine->stop();
        if(m_thread->joinable()) m_thread->join();
        return true;
    }

    bool EventScheduler::notify()
    {
        return m_engine->notify();
    }

    bool EventScheduler::isRunning() const
    {
        return m_engine->isRunning();
    }

    EventScheduler::error_ptr EventScheduler::getError() const
    {
        return m_engine->getError();
    }

    void EventScheduler::addTimer(timer_ptr timer, int64_t ms)
    {
        m_timer_event->addTimer(timer, ms);
    }

}