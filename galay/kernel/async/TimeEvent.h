#ifndef GALAY_TIME_EVENT_H
#define GALAY_TIME_EVENT_H 

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/kernel/time/TimerManager.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "galay/kernel/sync/SinglePassGate.h"
#include "galay/common/Common.h"

namespace galay::details
{
    
    template<CoType T>
    class TimeEvent: public AsyncEvent<T>
    {
    public:
        using ptr = std::shared_ptr<TimeEvent>;
        using wptr = std::weak_ptr<TimeEvent>;

        TimeEvent(TimerManager* manager) 
            :m_manager(manager) {} 
        std::string name() override { return "TimeEvent"; }
        EventType getEventType() const override { return kEventTypeNone; };
        GHandle getHandle() override { return {}; }
        void handleEvent() override {}
        bool suspend(Waker waker) override
        {
            this->m_waker = waker;
            return true;
        }
    protected:
        TimerManager* m_manager;
    };

    template <CoType T>
    class TimeoutEvent: public TimeEvent<ValueWrapper<T>> {
    public:
        TimeoutEvent(TimerManager* manager, const std::function<AsyncResult<T>()>& func, std::chrono::milliseconds ms)
            : TimeEvent<ValueWrapper<T>>(manager), m_ms(ms), m_func(func) {}
        std::string name() override { return "TimeoutEvent"; }
        bool ready() override {
            return false;
        }

        void handleEvent() override {}

        bool suspend(Waker waker) override {
            SinglePassGate::ptr gate = std::make_shared<SinglePassGate>();
            //不能在协程捕获列表捕获，会销毁(initial_suspend:: always)
            auto co = [](SinglePassGate::ptr gate, Waker waker, TimeoutEvent* event, std::function<AsyncResult<T>()>&& func) mutable -> Coroutine<nil> {
                T res = co_await func();
                if(gate->pass()) {
                    makeValue(event->m_result, std::move(res), nullptr);
                    waker.wakeUp();
                }
                co_return nil();
            };
            Coroutine<nil> res = co(gate, waker, this, std::move(m_func));
            CoroutineBase::wptr origin = res.getOriginCoroutine();
            waker.belongScheduler()->schedule(std::move(res));
            auto func = [gate, waker, origin, this]() mutable {
                using namespace error;
                if(gate->pass()) {
                    Error::ptr error = std::make_shared<SystemError>(ErrorCode::AsyncTimeoutError, 0);
                    makeValue(this->m_result, T{}, error);
                    waker.wakeUp();
                }
            };
            Timer::ptr timer = std::make_shared<Timer>(m_ms, func);
            this->m_manager->push(timer);
            return TimeEvent<ValueWrapper<T>>::suspend(waker);
        }
    protected:
        std::chrono::milliseconds m_ms;
        std::function<AsyncResult<T>()> m_func;  
    };

    class SleepforEvent: public TimeEvent<nil> 
    {
    public:
        SleepforEvent(TimerManager* manager, std::chrono::milliseconds ms);
        std::string name() override { return "SleepforEvent"; }
        void handleEvent() override {}

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        std::chrono::milliseconds m_ms;
    };

}


#endif