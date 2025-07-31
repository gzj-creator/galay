//
// Created by gong on 2025/6/28.
//

#ifndef GALAY_RUNTIME_H
#define GALAY_RUNTIME_H

#include "galay/kernel/coroutine/CoroutineScheduler.hpp"

namespace galay
{

    class Runtime
    {
    public:
        Runtime();
        void modifyTimerManagerType(TimerManagerType type);

        template<CoType T>
        void schedule(Coroutine<T>&& co);
        ~Runtime();
    private:
        CoroutineScheduler::uptr m_scheduler;
    };

     template<CoType T>
     inline void Runtime::schedule(Coroutine<T>&& co)
     {
         m_scheduler->schedule(std::forward<Coroutine<T>>(co));
     }

}



#endif //GALAY_RUNTIME_H