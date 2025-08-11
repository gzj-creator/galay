#include "EventScheduler.h"
#include "EventEngine.h"
#include "galay/common/Log.h"
#include "Event.h"

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
        event->getHandle().flags[0] = 1;
        return m_engine->addEvent(event, ctx) == 0;
    }


    bool EventScheduler::modEvent(details::Event* event, void* ctx)
    {
        return m_engine->modEvent(event, ctx) == 0;
    }

    bool EventScheduler::delEvent(details::Event* event, void* ctx)
    {
        event->getHandle().flags[0] = 0;
        return m_engine->delEvent(event, ctx) == 0;
    }

    void EventScheduler::registerOnceLoopCallback(const std::function<void()>& callback)
    {
        m_engine->registerOnceLoopCallback(callback);
    }

    bool EventScheduler::start(int timeout)
    {
        this->m_thread = std::make_unique<std::thread>([this, timeout](){
            m_engine->start(timeout);
            LogTrace("[{}({}) exist successfully]", name(), GetEngine()->getHandle().fd);
        });
        return true;
    }

    bool EventScheduler::stop()
    {
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

}