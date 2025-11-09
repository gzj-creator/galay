#ifndef GALAY_EVENT_ENGINE_H
#define GALAY_EVENT_ENGINE_H
#include "galay/common/Base.h"
#include "galay/common/Error.h"
#include "EventDispatch.h"
#include <libcuckoo/cuckoohash_map.hh>
#include <string>
#include <atomic>
#include <list>
#include <memory>
#include <functional>
#include <optional>

namespace galay::details
{
    class Event;

    #define DEFAULT_MAX_EVENTS 1024

    using namespace error;

    class EventEngine
    {
        static std::atomic_uint32_t gEngineId;
    public:
        using ptr = std::shared_ptr<EventEngine>;
        EventEngine();
        uint32_t getEngineID() const { return m_id.load(); }
        virtual bool start(int timeout) = 0;
        virtual bool stop() = 0;
        virtual bool notify() = 0;
        virtual int addEvent(Event* event, void* ctx) = 0;
        virtual int modEvent(Event* event, void* ctx) = 0;
        virtual int delEvent(Event* event, void* ctx) = 0;
        virtual bool isRunning() const = 0;
        virtual std::optional<CommonError> getError() = 0;
        virtual GHandle getHandle() = 0;
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
        EpollEventEngine(uint32_t max_events = DEFAULT_MAX_EVENTS);
        bool start(int timeout = -1) override;
        bool stop() override;
        bool notify() override;
        int addEvent(Event* event, void* ctx) override;
        int modEvent(Event* event, void* ctx) override;
        int delEvent(Event* event, void* ctx) override;
        bool isRunning() const override { return !m_stop; }
        std::optional<CommonError> getError() override { return m_error; }
        GHandle getHandle() override { return m_handle; }
        void registerOnceLoopCallback(const std::function<void()>& callback) override { m_once_loop_cbs.push_back(callback); }
        ~EpollEventEngine() override;
    private:
        bool convertToEpollEvent(struct epoll_event &ev, Event *event, void* ctx);
    private:
        GHandle m_handle;
        std::optional<CommonError> m_error;
        std::atomic_bool m_stop;
        std::atomic_uint32_t m_event_size;
        std::atomic<epoll_event*> m_events;
        std::list<std::function<void()>> m_once_loop_cbs;
        libcuckoo::cuckoohash_map<int, std::shared_ptr<galay::EventDispatcher>> m_dispatchers;
    };
    #elif defined(USE_IOURING)
    // class IOUringEventEngine final: public EventEngine { 
    // public:
    //     //对实时性要求低的应用可以使用batch_mode提高并发
    //     explicit IOUringEventEngine(bool batch_mode, uint32_t max_events = DEFAULT_MAX_EVENTS);
    //     bool start(int timeout) override;
    //     bool stop() override;
    //     bool notify() override;
    //     virtual int addEvent(Event* event, void* ctx) = 0;
    //     virtual int modEvent(Event* event, void* ctx) = 0;
    //     virtual int delEvent(Event* event, void* ctx) = 0;
    //     virtual bool isRunning() const = 0;
    //     virtual error_ptr getError() const = 0;
    //     virtual GHandle getHandle() = 0;
    //     virtual void registerOnceLoopCallback(const std::function<void()>& callback) = 0;
    //     ~IOUringEventEngine() override;
    // private: 
    //     void startWait(int timeout);
    //     void startBatch(int timeout);
    // private:
    //     uint32_t m_max_events = 0;
    //     error_ptr m_error;
    //     std::atomic_bool m_stop;
    //     bool m_batch_mode = false;

    //     io_uring m_ring;
    // };
    #elif defined(USE_SELECT)

    #elif defined(USE_KQUEUE)

    //default ET 
    class KqueueEventEngine final : public EventEngine
    {
    public:
        explicit KqueueEventEngine(uint32_t max_events = DEFAULT_MAX_EVENTS);
        bool start(int timeout = -1) override;
        bool stop() override;
        bool notify() override;
        int addEvent(Event* event, void* ctx ) override;
        int modEvent(Event* event, void* ctx) override;
        int delEvent(Event* event, void* ctx) override;
        bool isRunning() const override { return !m_stop; }
        std::optional<CommonError> getError() override { return m_error; }
        GHandle getHandle() override { return m_handle; }
        void registerOnceLoopCallback(const std::function<void()>& callback) override { m_once_loop_cbs.push_back(callback); }
        ~KqueueEventEngine() override;
    private:
        bool convertToKEvent(struct kevent &ev, Event *event, void* ctx);
    private:
        GHandle m_handle{};
        std::optional<CommonError> m_error;
        std::atomic_bool m_stop;
        std::atomic_uint32_t m_event_size;
        std::atomic<struct kevent*> m_events;
        std::list<std::function<void()>> m_once_loop_cbs;
        libcuckoo::cuckoohash_map<int, std::shared_ptr<galay::EventDispatcher>> m_dispatchers;
    };

    #endif





}

#endif