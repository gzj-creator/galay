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
        ~EventScheduler() = default;
    protected:
        std::unique_ptr<std::thread> m_thread;
        std::shared_ptr<details::EventEngine> m_engine;
    };

}


#endif