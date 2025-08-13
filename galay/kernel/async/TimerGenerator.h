#ifndef GALAY_TIME_EXECUTOR_H
#define GALAY_TIME_EXECUTOR_H 


#include "TimeEvent.h"
#include "galay/kernel/runtime/Runtime.h"

namespace galay 
{ 
    //不允许跨线程/协程同时调用同一个TimerGenerator对象
    class TimerGenerator 
    {
    public:
        TimerGenerator(Runtime& runtime);
        template <CoType T>
        AsyncResult<ValueWrapper<T>> timeout(std::chrono::milliseconds ms, const std::function<AsyncResult<T>()>& func);
        AsyncResult<nil> sleepfor(std::chrono::milliseconds ms);
    private:
        TimerManager* m_manager = nullptr;
    };

    template <CoType T>
    inline AsyncResult<ValueWrapper<T>> TimerGenerator::timeout(std::chrono::milliseconds ms, const std::function<AsyncResult<T>()> &func)
    {
        return {std::make_shared<details::TimeoutEvent<T>>(m_manager, func, ms)};
    }

}

#endif