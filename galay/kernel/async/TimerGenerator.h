#ifndef GALAY_TIME_EXECUTOR_H
#define GALAY_TIME_EXECUTOR_H 


#include "TimeEvent.h"
#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include "galay/kernel/runtime/Runtime.h"

namespace galay 
{ 
    //不允许跨线程/协程同时调用同一个TimerGenerator对象
    class TimerGenerator 
    {
    public:
        using ptr = std::shared_ptr<TimerGenerator>;
        static TimerGenerator::ptr createPtr(Runtime& runtime, int co_id = -1);
        TimerGenerator(Runtime& runtime, int co_id = -1);
        template <CoType T>
        AsyncResult<std::expected<T, CommonError>> timeout(std::chrono::milliseconds ms, const std::function<AsyncResult<T>()>& func);
        AsyncResult<nil> sleep(std::chrono::milliseconds ms);
        TimerGenerator(const TimerGenerator& other);
        TimerGenerator& operator=(const TimerGenerator& other) = delete;
    private:
        template <CoType T>
        Coroutine<nil> waitSleep(std::chrono::milliseconds ms, std::shared_ptr<AsyncResultWaiter<T, CommonError>> waiter);
        template <CoType T>
        Coroutine<nil> waitFunc(const std::function<AsyncResult<T>()>& func, std::shared_ptr<AsyncResultWaiter<T, CommonError>> waiter);
    private:
        Runtime& m_runtime;
        Timer::ptr m_timer;
        int m_co_id;
    };

    template <CoType T>
    inline AsyncResult<std::expected<T, CommonError>> TimerGenerator::timeout(std::chrono::milliseconds ms, const std::function<AsyncResult<T>()> &func)
    {
        std::shared_ptr<AsyncResultWaiter<T, CommonError>> waiter = std::make_shared<AsyncResultWaiter<T, CommonError>>();
        if(m_co_id == -1) {
            m_runtime.schedule(waitSleep<T>(ms, waiter));
            m_runtime.schedule(waitFunc<T>(func, waiter));
        } else {
            m_runtime.schedule(waitSleep<T>(ms, waiter), m_co_id);
            m_runtime.schedule(waitFunc<T>(func, waiter), m_co_id);
        }
        return waiter->wait();
    }

    template <CoType T>
    Coroutine<nil> TimerGenerator::waitSleep(std::chrono::milliseconds ms, std::shared_ptr<AsyncResultWaiter<T, CommonError>> waiter)
    {
        co_await this->sleep(ms);
        if(waiter->isWaiting()) {
            using namespace error;
            waiter->notify(std::unexpected(CommonError(AsyncTimeoutError, static_cast<uint32_t>(errno))));
        }
        co_return nil();
    }

    template <CoType T>
    inline Coroutine<nil> TimerGenerator::waitFunc(const std::function<AsyncResult<T>()>& func, std::shared_ptr<AsyncResultWaiter<T, CommonError>> waiter)
    {
        T res = co_await func();
        if(waiter->isWaiting()) {
            waiter->notify(std::move(res));
        }
        co_return nil();
    }
}

#endif