#include "EventScheduler.h"
#include "EventEngine.h"
#include "galay/common/Log.h"
#include "Event.h"

namespace galay{ 

    EventScheduler::EventScheduler(int64_t fds_init_size)
        :m_fds(fds_init_size)
    {
    #if defined(USE_EPOLL)
        m_engine = std::make_shared<details::EpollEventEngine>();
    #elif defined(USE_IOURING)
        m_engine = std::make_shared<details::IOUringEventEngine>();
    #elif defined(USE_KQUEUE)
        m_engine = std::make_shared<details::KqueueEventEngine>();
    #endif
    }

    EventScheduler::EventScheduler(engine_ptr engine, int64_t fds_init_size)
        : m_fds(fds_init_size), m_engine(engine)
    {
    }

    bool EventScheduler::activeEvent(details::Event* event, void* ctx)
    {
        if(!m_fds.contains(event->getHandle().fd)) {
            m_fds.insert(event->getHandle().fd, std::monostate());
            return m_engine->addEvent(event, ctx) == 0;
        }
        return m_engine->modEvent(event, ctx) == 0;
    }

    bool EventScheduler::removeEvent(details::Event* event, void* ctx)
    {
        if(!m_fds.contains(event->getHandle().fd)) {
            return false;
        }
        m_fds.erase(event->getHandle().fd);
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
            LogTrace("[{}({}) exist successfully]", name(), m_engine->getEngineID());
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