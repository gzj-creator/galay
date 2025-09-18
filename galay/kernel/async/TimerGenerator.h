#ifndef GALAY_TIME_EXECUTOR_H
#define GALAY_TIME_EXECUTOR_H 


#include "TimeEvent.h"
#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include "galay/kernel/runtime/Runtime.h"

namespace galay 
{ 
    class TimerGenerator 
    {
    public:
        using ptr = std::shared_ptr<TimerGenerator>;
        static TimerGenerator::ptr createPtr(Runtime& runtime);
        TimerGenerator(Runtime& runtime);
        AsyncResult<nil> wait(std::chrono::milliseconds ms, const std::function<void()>& func);
        template <CoType T>
        AsyncResult<std::expected<T, CommonError>> timeout(Holder& holder, const std::function<AsyncResult<T>()>& func, std::chrono::milliseconds ms);
        AsyncResult<nil> sleep(std::chrono::milliseconds ms);
    private:
        template <CoType T>
        Coroutine<nil> waitSleep(std::chrono::milliseconds ms, std::shared_ptr<AsyncResultWaiter<T, CommonError>> waiter);
        template <CoType T>
        Coroutine<nil> waitFunc(const std::function<AsyncResult<T>()>& func, std::shared_ptr<AsyncResultWaiter<T, CommonError>> waiter);
    private:
        Runtime& m_runtime;
    };

    template <CoType T>
    inline AsyncResult<std::expected<T, CommonError>> TimerGenerator::timeout(Holder& holder, const std::function<AsyncResult<T>()> &func, std::chrono::milliseconds ms)
    {
        std::shared_ptr<AsyncResultWaiter<T, CommonError>> waiter = std::make_shared<AsyncResultWaiter<T, CommonError>>();
        m_runtime.schedule(waitSleep<T>(ms, waiter));
        holder = m_runtime.schedule(waitFunc<T>(func, waiter));
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