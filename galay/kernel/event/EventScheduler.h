#ifndef GALAY_EVENT_SCHEDULER_H
#define GALAY_EVENT_SCHEDULER_H 

#include <thread>
#include <memory>
#include <string>
#include <variant>
#include <functional>
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <libcuckoo/cuckoohash_map.hh>
#ifdef __clang__
    #pragma clang diagnostic pop
#elif defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
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

        EventScheduler(int64_t fds_init_size);
        EventScheduler(engine_ptr engine, int64_t fds_init_size);
        std::string name() override { return "EventScheduler"; }

        bool activeEvent(details::Event* event, void* ctx);
        bool removeEvent(details::Event* event, void* ctx);

        void registerOnceLoopCallback(const std::function<void()>& callback);
        bool start(int timeout);
        bool stop();
        bool notify();
        bool isRunning() const;
        std::optional<CommonError> getError() const;
        ~EventScheduler() = default;
    protected:
        std::unique_ptr<std::thread> m_thread;
        libcuckoo::cuckoohash_map<int, std::monostate> m_fds;
        std::shared_ptr<details::EventEngine> m_engine;
    };

}


#endif