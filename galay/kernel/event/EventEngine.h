#ifndef GALAY_EVENT_ENGINE_H
#define GALAY_EVENT_ENGINE_H
#include "galay/common/Base.h"
#include "galay/common/Error.h"
#include <string>
#include <atomic>
#include <list>
#include <memory>
#include <functional>

namespace galay::details
{
class Event;

#define DEFAULT_MAX_EVENTS 1024


class EventEngine
{
    static std::atomic_uint32_t gEngineId;
public:

    using ptr = std::shared_ptr<EventEngine>;
    using error_ptr = error::Error::ptr;
    EventEngine();

    uint32_t GetEngineID() const { return m_id.load(); }
    virtual bool start(int timeout) = 0;
    virtual bool stop() = 0;
    virtual bool notify() = 0;
    virtual int addEvent(Event* event, void* ctx) = 0;
    virtual int modEvent(Event* event, void* ctx) = 0;
    virtual int delEvent(Event* event, void* ctx) = 0;
    virtual bool isRunning() const = 0;
    virtual error_ptr getError() const = 0;
    virtual GHandle getHandle() = 0;
    virtual uint32_t getMaxEventSize() = 0;
    virtual void resetMaxEventSize(uint32_t size) = 0;
    virtual void registerOnceLoopCallback(const std::function<void()>& callback) = 0;
    virtual ~EventEngine() = default;
protected:
    std::atomic_uint32_t m_id;
};

#if defined(USE_EPOLL)
//default ET mode
class EpollEventEngine: public EventEngine
{
public:
    using error_ptr = error::Error::ptr;
    EpollEventEngine(uint32_t max_events = DEFAULT_MAX_EVENTS);
    bool start(int timeout = -1) override;
    bool stop() override;
    bool notify() override;
    int addEvent(Event* event, void* ctx) override;
    int modEvent(Event* event, void* ctx) override;
    int delEvent(Event* event, void* ctx) override;
    bool isRunning() const override { return !m_stop; }
    error_ptr getError() const override { return m_error; }
    GHandle getHandle() override { return m_handle; }
    uint32_t getMaxEventSize() override { return m_event_size; }
    void registerOnceLoopCallback(const std::function<void()>& callback) override { m_once_loop_cbs.push_back(callback); }

    /*
        设置步骤
            1.if size > m_event_size, then m_event_size *= 2;
            2.if size < m_event_size / 4, then m_event /= 2;
            3.m_event_size >= DEFAULT_MAX_EVENTS
    */
    void resetMaxEventSize(uint32_t size) override;
    ~EpollEventEngine() override;
private:
    bool convertToEpollEvent(struct epoll_event &ev, Event *event, void* ctx);
private:
    GHandle m_handle;
    error_ptr m_error;
    std::atomic_bool m_stop;
    std::atomic_uint32_t m_event_size;
    std::atomic<epoll_event*> m_events;
    std::list<std::function<void()>> m_once_loop_cbs; 
};
#elif defined(USE_IOURING)
class IoUringEventEngine
{
    
};

#elif defined(USE_SELECT)

#elif defined(USE_KQUEUE)

//default ET 
class KqueueEventEngine final : public EventEngine
{
public:
    using error_ptr = error::Error::ptr;
    explicit KqueueEventEngine(uint32_t max_events = DEFAULT_MAX_EVENTS);
    bool start(int timeout = -1) override;
    bool stop() override;
    bool notify() override;
    int addEvent(Event* event, void* ctx ) override;
    int modEvent(Event* event, void* ctx) override;
    int delEvent(Event* event, void* ctx) override;
    bool isRunning() const override { return !m_stop; }
    error_ptr getError() const override { return m_error; }
    GHandle getHandle() override { return m_handle; }
    uint32_t getMaxEventSize() override { return m_event_size; }
    void resetMaxEventSize(const uint32_t size) override { m_event_size = size; }
    void registerOnceLoopCallback(const std::function<void()>& callback) override { m_once_loop_cbs.push_back(callback); }
    ~KqueueEventEngine() override;
private:
    bool convertToKEvent(struct kevent &ev, Event *event, void* ctx);
private:
    GHandle m_handle{};
    error_ptr m_error;
    std::atomic_bool m_stop;
    std::atomic_uint32_t m_event_size;
    std::atomic<struct kevent*> m_events;
    std::list<std::function<void()>> m_once_loop_cbs; 
};

#endif





}

#endif