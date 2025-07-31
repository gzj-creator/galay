#include "CoApi.hpp"
#include "CoroutineScheduler.hpp"
#include "galay/kernel/event/Timer.hpp"

namespace galay
{

    AsyncResult<CoroutineBase::wptr> GetThisCoroutine()
    {
        return {std::make_shared<details::GetCoEvent>()};
    }


    AsyncResult<nil> Sleepfor(int64_t ms)
    {
        return {std::make_shared<details::SleepforEvent>(ms)};
    }

}