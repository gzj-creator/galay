#include "EventScheduler.h"
#include "EventEngine.h"
#include "galay/common/Common.h"
#include "galay/common/Log.h"
#include "Event.h"
#include <pthread.h>

namespace galay{

#if !defined(USE_IOURING)
    // ReactorEventScheduler 实现
    ReactorEventScheduler::ReactorEventScheduler()
    {
    #if defined(USE_EPOLL)
        m_engine = std::make_shared<details::EpollEventEngine>();
    #elif defined(USE_KQUEUE)
        m_engine = std::make_shared<details::KqueueEventEngine>();
    #endif
    }

    ReactorEventScheduler::ReactorEventScheduler(engine_ptr engine)
        : m_engine(engine)
    {
    }

    bool ReactorEventScheduler::activeEvent(details::Event* event, void* ctx)
    {
        return m_engine->addEvent(event, ctx) == 0;
    }

    bool ReactorEventScheduler::removeEvent(details::Event* event, void* ctx)
    {
        return m_engine->delEvent(event, ctx) == 0;
    }

    void ReactorEventScheduler::registerOnceLoopCallback(const std::function<void()>& callback)
    {
        m_engine->registerOnceLoopCallback(callback);
    }

    bool ReactorEventScheduler::start(int timeout)
    {
        if (m_engine->isRunning()) {
            return false;
        }
        this->m_thread = std::make_unique<std::thread>([this, timeout](){
            setThreadName("ReactorEventScheduler");
            m_engine->start(timeout);
            LogTrace("[{}({}) exist successfully]", name(), m_engine->getEngineID());
        });
        return true;
    }

    bool ReactorEventScheduler::stop()
    {
        if(!m_engine->isRunning()) return false;
        m_engine->stop();
        if(m_thread->joinable()) m_thread->join();
        return true;
    }

    bool ReactorEventScheduler::notify()
    {
        return m_engine->notify();
    }

    bool ReactorEventScheduler::isRunning() const
    {
        return m_engine->isRunning();
    }

    std::optional<CommonError> ReactorEventScheduler::getError() const
    {
        return m_engine->getError();
    }

#else
    // ProactorEventScheduler 实现
    ProactorEventScheduler::ProactorEventScheduler()
    {
        m_engine = std::make_shared<details::IOUringEventEngine>();
    }

    ProactorEventScheduler::ProactorEventScheduler(engine_ptr engine)
        : m_engine(engine)
    {
    }

    void ProactorEventScheduler::registerOnceLoopCallback(const std::function<void()>& callback)
    {
        m_engine->registerOnceLoopCallback(callback);
    }

    bool ProactorEventScheduler::start(int timeout)
    {
        if (m_engine->isRunning()) {
            return false;
        }
        this->m_thread = std::make_unique<std::thread>([this, timeout](){
            setThreadName("ProactorEventScheduler");
            m_engine->start(timeout);
            LogTrace("[{}({}) exist successfully]", name(), m_engine->getEngineID());
        });
        return true;
    }

    bool ProactorEventScheduler::stop()
    {
        if(!m_engine->isRunning()) return false;
        m_engine->stop();
        if(m_thread->joinable()) m_thread->join();
        return true;
    }

    bool ProactorEventScheduler::notify()
    {
        return m_engine->notify();
    }

    bool ProactorEventScheduler::isRunning() const
    {
        return m_engine->isRunning();
    }

    std::optional<CommonError> ProactorEventScheduler::getError() const
    {
        return m_engine->getError();
    }

    bool ProactorEventScheduler::submitRead(details::Event* event, int fd, void* buf, size_t len)
    {
        return m_engine->submitRead(event, fd, buf, len) == 0;
    }

    bool ProactorEventScheduler::submitWrite(details::Event* event, int fd, const void* buf, size_t len)
    {
        return m_engine->submitWrite(event, fd, buf, len) == 0;
    }

    bool ProactorEventScheduler::submitRecv(details::Event* event, int fd, void* buf, size_t len, int flags)
    {
        return m_engine->submitRecv(event, fd, buf, len, flags) == 0;
    }

    bool ProactorEventScheduler::submitSend(details::Event* event, int fd, const void* buf, size_t len, int flags)
    {
        return m_engine->submitSend(event, fd, buf, len, flags) == 0;
    }

    bool ProactorEventScheduler::submitAccept(details::Event* event, int fd, sockaddr* addr, socklen_t* addrlen)
    {
        return m_engine->submitAccept(event, fd, addr, addrlen) == 0;
    }

    bool ProactorEventScheduler::submitConnect(details::Event* event, int fd, const sockaddr* addr, socklen_t addrlen)
    {
        return m_engine->submitConnect(event, fd, addr, addrlen) == 0;
    }

    bool ProactorEventScheduler::submitClose(details::Event* event, int fd)
    {
        return m_engine->submitClose(event, fd) == 0;
    }

    bool ProactorEventScheduler::submitRecvfrom(details::Event* event, int fd, void* buf, size_t len, int flags,
                                        sockaddr* src_addr, socklen_t* addrlen)
    {
        return m_engine->submitRecvfrom(event, fd, buf, len, flags, src_addr, addrlen) == 0;
    }

    bool ProactorEventScheduler::submitSendto(details::Event* event, int fd, const void* buf, size_t len, int flags,
                                      const sockaddr* dest_addr, socklen_t addrlen)
    {
        return m_engine->submitSendto(event, fd, buf, len, flags, dest_addr, addrlen) == 0;
    }
#endif

}