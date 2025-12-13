#ifndef GALAY_EVENT_SCHEDULER_H
#define GALAY_EVENT_SCHEDULER_H

#include <thread>
#include <memory>
#include <string>
#include <variant>
#include <functional>
#include "galay/common/Base.h"
#include "galay/common/Error.h"
#include <optional>

namespace galay::details{
    class EventEngine;
    class Event;
}

namespace galay
{
    using namespace error;
    class Timer;

    class Scheduler
    {
    public:
        virtual std::string name() = 0;
        virtual ~Scheduler() = default;
    };

    class EventScheduler final: public Scheduler
    {
    public:
        using ptr = std::shared_ptr<EventScheduler>;
        using uptr = std::unique_ptr<EventScheduler>;

        using timer_ptr = std::shared_ptr<Timer>;
        using engine_ptr = std::shared_ptr<details::EventEngine>;

        EventScheduler();
        EventScheduler(engine_ptr engine);
        std::string name() override { return "EventScheduler"; }

        bool activeEvent(details::Event* event, void* ctx);
        bool removeEvent(details::Event* event, void* ctx);

        void registerOnceLoopCallback(const std::function<void()>& callback);
        bool start(int timeout);
        bool stop();
        bool notify();
        bool isRunning() const;
        std::optional<CommonError> getError() const;

#if defined(USE_IOURING)
        // io_uring Proactor 风格接口
        bool submitRead(details::Event* event, int fd, void* buf, size_t len);
        bool submitWrite(details::Event* event, int fd, const void* buf, size_t len);
        bool submitRecv(details::Event* event, int fd, void* buf, size_t len, int flags);
        bool submitSend(details::Event* event, int fd, const void* buf, size_t len, int flags);
        bool submitAccept(details::Event* event, int fd, sockaddr* addr, socklen_t* addrlen);
        bool submitConnect(details::Event* event, int fd, const sockaddr* addr, socklen_t addrlen);
        bool submitClose(details::Event* event, int fd);
        bool submitRecvfrom(details::Event* event, int fd, void* buf, size_t len, int flags,
                           sockaddr* src_addr, socklen_t* addrlen);
        bool submitSendto(details::Event* event, int fd, const void* buf, size_t len, int flags,
                         const sockaddr* dest_addr, socklen_t addrlen);
#endif

        ~EventScheduler() = default;
    protected:
        std::unique_ptr<std::thread> m_thread;
        std::shared_ptr<details::EventEngine> m_engine;
    };

}


#endif