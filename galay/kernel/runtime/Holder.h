#ifndef GALAY_HOLDER_H
#define GALAY_HOLDER_H 

#include "galay/kernel/coroutine/CoScheduler.hpp"

namespace galay 
{
    class Holder
    {
    public:
        Holder();
        Holder(CoroutineScheduler* scheduler, int index, CoroutineBase::wptr co);
        void destory();
        
        CoroutineScheduler* scheduler();
        CoroutineBase::wptr coroutine(); 
        int index();
    private:
        int m_index;
        CoroutineBase::wptr m_co;
        CoroutineScheduler* m_scheduler;
    };
}


#endif