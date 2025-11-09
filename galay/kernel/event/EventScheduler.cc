#include "EventScheduler.h"
#include "EventEngine.h"
#include "galay/common/Common.h"
#include "galay/common/Log.h"
#include "Event.h"
#include <pthread.h>

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

    bool EventScheduler::activeEvent(details::Event* event, void* ctx)
    {
        // 直接委托给 EventEngine 处理
        // EventEngine 会管理 dispatcher 并判断是 add 还是 mod
        return m_engine->addEvent(event, ctx) == 0;
    }

    bool EventScheduler::removeEvent(details::Event* event, void* ctx)
    {
        // 直接委托给 EventEngine 处理
        // EventEngine 会管理 dispatcher 并判断是 del 还是 mod
        return m_engine->delEvent(event, ctx) == 0;
    }

    void EventScheduler::registerOnceLoopCallback(const std::function<void()>& callback)
    {
        m_engine->registerOnceLoopCallback(callback);
    }

    bool EventScheduler::start(int timeout)
    {
        if (m_engine->isRunning()) {
            return false;
        }        
        this->m_thread = std::make_unique<std::thread>([this, timeout](){
            setThreadName("EventScheduler");
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

    std::optional<CommonError> EventScheduler::getError() const
    {
        return m_engine->getError();
    }

}