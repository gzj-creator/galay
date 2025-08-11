#ifndef GALAY_TIME_EXECUTOR_H
#define GALAY_TIME_EXECUTOR_H 


#include "TimeEvent.h"

namespace galay 
{ 
    //不允许跨线程/协程同时调用同一个TimerGenerator对象
    class TimerGenerator 
    {
    public:
        TimerGenerator(EventScheduler* scheduler);

        template <typename T>
        AsyncResult<ValueWrapper<T>> timeout(std::chrono::milliseconds ms, const std::function<AsyncResult<T>()>& func);
        AsyncResult<ValueWrapper<bool>> close();
        ~TimerGenerator();
    private:
        details::TimeStatusContext m_context;
    };

    // template <typename T>
    // inline AsyncResult<ValueWrapper<T>> TimerGenerator::timeout(std::chrono::milliseconds ms, const std::function<AsyncResult<T>()>& func)
    // {
    //     return {std::make_shared<>}
    // }

}

#endif