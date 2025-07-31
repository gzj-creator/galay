#ifndef GALAY_CO_API_HPP
#define GALAY_CO_API_HPP 


#include <chrono>
#include <functional>
#include "CoEvent.hpp"
#include "CoroutineScheduler.hpp"
#include "galay/kernel/event/Timer.hpp"

namespace galay
{

    AsyncResult<CoroutineBase::wptr> GetThisCoroutine();


    /*
        return false only when TimeScheduler is not running
        [ms] : ms
        [timer] : timer
        [scheduler] : coroutine_scheduler, this coroutine will resume at this scheduler
    */
    AsyncResult<nil> Sleepfor(int64_t ms);

    

    template<CoType T>
    inline AsyncResult<SyncWrapper<T>> Timeout(std::chrono::milliseconds ms, const std::function<AsyncResult<T>()>& func)
    {
       return {std::make_shared<details::TimeoutEvent<SyncWrapper<T>>>(ms, func)};
    }

    

}

#endif