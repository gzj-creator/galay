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
    class ReactorEventEngine;
    class ProactorEventEngine;
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

#if !defined(USE_IOURING)
    // Reactor 模式调度器 (epoll, kqueue)
    class ReactorEventScheduler final: public Scheduler
    {
    public:
        using ptr = std::shared_ptr<ReactorEventScheduler>;
        using uptr = std::unique_ptr<ReactorEventScheduler>;
        using timer_ptr = std::shared_ptr<Timer>;
        using engine_ptr = std::shared_ptr<details::ReactorEventEngine>;

        ReactorEventScheduler();
        ReactorEventScheduler(engine_ptr engine);
        std::string name() override { return "ReactorEventScheduler"; }

        bool activeEvent(details::Event* event, void* ctx);
        bool removeEvent(details::Event* event, void* ctx);

        void registerOnceLoopCallback(const std::function<void()>& callback);
        bool start(int timeout);
        bool stop();
        bool notify();
        bool isRunning() const;
        std::optional<CommonError> getError() const;

        ~ReactorEventScheduler() = default;
    protected:
        std::unique_ptr<std::thread> m_thread;
        std::shared_ptr<details::ReactorEventEngine> m_engine;
    };

    // 为了兼容性，定义 EventScheduler 为 ReactorEventScheduler 的别名
    using EventScheduler = ReactorEventScheduler;

#else
    // Proactor 模式调度器 (io_uring)
    class ProactorEventScheduler final: public Scheduler
    {
    public:
        using ptr = std::shared_ptr<ProactorEventScheduler>;
        using uptr = std::unique_ptr<ProactorEventScheduler>;
        using timer_ptr = std::shared_ptr<Timer>;
        using engine_ptr = std::shared_ptr<details::ProactorEventEngine>;

        ProactorEventScheduler();
        ProactorEventScheduler(engine_ptr engine);
        std::string name() override { return "ProactorEventScheduler"; }

        void registerOnceLoopCallback(const std::function<void()>& callback);
        bool start(int timeout);
        bool stop();
        bool notify();
        bool isRunning() const;
        std::optional<CommonError> getError() const;

        // Proactor 风格接口
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

        ~ProactorEventScheduler() = default;
    protected:
        std::unique_ptr<std::thread> m_thread;
        std::shared_ptr<details::ProactorEventEngine> m_engine;
    };

    // 为了兼容性，定义 EventScheduler 为 ProactorEventScheduler 的别名
    using EventScheduler = ProactorEventScheduler;
#endif

}


#endif