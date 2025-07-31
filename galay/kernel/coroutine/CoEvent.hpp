#ifndef GALAY_COEVENT_HPP
#define GALAY_COEVENT_HPP

#include "Result.hpp"
#include "CoroutineScheduler.hpp"
#include "galay/kernel/event/Timer.hpp"

namespace galay
{
    
template<CoType T>
class SyncWrapper
{
public:
    SyncWrapper() = default; // 默认构造函数
    SyncWrapper(const SyncWrapper& other) : m_value(other.m_value) {} // 拷贝构造函数
    SyncWrapper(SyncWrapper&& other) noexcept : m_value(std::move(other.m_value)) {} // 移动构造函数
    bool pass();
    void setValue(T&& value);
    std::optional<T>& Value();
private:
    std::optional<T> m_value;
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
};

template<CoType T>
bool SyncWrapper<T>::pass()
{
    return !m_flag.test_and_set();
}

template<CoType T>
void SyncWrapper<T>::setValue(T&& value)
{
    m_value = std::forward<T>(value);
}

template<CoType T>
std::optional<T>& SyncWrapper<T>::Value()
{
    return m_value;
}
}

namespace galay::details
{ 

#define DEFAULT_ASYNC_EVENT_IMPL() \
    void handleEvent() override {} \
    EventType getEventType() override { return EventType::kEventTypeNone; }\
    GHandle getHandle() override { return {-1}; } \
    bool setEventScheduler(EventScheduler* scheduler) override { return true; } \
    EventScheduler* belongEventScheduler() override { return nullptr; }


class GetCoEvent: public AsyncEvent<CoroutineBase::wptr>
{ 
public:
    GetCoEvent(): AsyncEvent<CoroutineBase::wptr>() {}
    bool ready() override { return true; }
    bool suspend(Waker waker) override {
        m_result = waker.getCoroutine();
        return false;
    }
private:
    std::string name() override { return "GetCoEvent"; }
    DEFAULT_ASYNC_EVENT_IMPL();

};

class SleepforEvent: public AsyncEvent<nil> 
{
public:
    SleepforEvent(int64_t ms) : AsyncEvent<nil>(), m_ms(ms) {
    } 

    bool ready() override {
        if(m_ms <= 0) return true;
        return false;
    }

    bool suspend(Waker waker) override { 
        auto co = waker.getCoroutine();
        if(co.expired() || co.lock()->belongScheduler() == nullptr) return false;
        Timer::ptr timer = Timer::create([this](auto event, auto timer) {
            m_waker.wakeUp();
        });
        co.lock()->belongScheduler()->getEventScheduler()->addTimer(timer, m_ms);
        return true;
    }
private:
    std::string name() override { return "SleepforEvent"; }
    DEFAULT_ASYNC_EVENT_IMPL();
private:
    int64_t m_ms;
};

template<CoType T>
class TimeoutEvent: public AsyncEvent<SyncWrapper<T>> { 
public:
    TimeoutEvent(std::chrono::milliseconds ms, const std::function<AsyncResult<T>()>& func)
        : AsyncEvent<SyncWrapper<T>>(), m_func(func), m_ms(ms) {}
    bool ready() override {
        if(m_ms <= 0) return true;
        return false;
    }

    bool suspend(Waker waker) override { 
        auto scheduler = waker.belongScheduler();
        auto co_fn = [this, waker]() mutable ->Coroutine<nil>
        {
            T res = co_await m_func();
            if (this->m_result.pass())
            {
                this->m_result.setValue(std::move(res));
                waker.wakeUp();
            }
            co_return nil();
        };
        auto sub_co = co_fn();
        auto sub_co_wptr = sub_co.getOriginCoroutine();
        scheduler->schedule(std::move(sub_co));
        auto time_cb = [this, waker, sub_co_wptr](auto event, auto timer) mutable
        {
            if (this->m_result.pass())
            {
                waker.wakeUp();
                if (!sub_co_wptr.expired()) sub_co_wptr.lock()->belongScheduler()->destroyCoroutine(sub_co_wptr);
            }
        };
        Timer::ptr timer = Timer::create(std::move(time_cb));
        waker.belongScheduler()->getEventScheduler()->addTimer(timer, static_cast<int64_t>(m_ms.count()));
        return true;
    }
private:
    std::string name() override { return "TimeoutEvent"; }
    DEFAULT_ASYNC_EVENT_IMPL();
private:
    std::function<AsyncResult<T>()> m_func;
    std::chrono::milliseconds m_ms;
};


}


#endif