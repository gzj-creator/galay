#include "Holder.h"

namespace galay 
{ 
    Holder::Holder()
        : m_index(-1), m_scheduler(nullptr)
    {
    }

    Holder::Holder(CoroutineScheduler *scheduler, int index, CoroutineBase::wptr co)
        : m_index(index), m_co(co), m_scheduler(scheduler)
    {
    }

    void Holder::destory()
    {
        if(!m_co.expired()) m_scheduler->destroyCoroutine(m_co);
    }

    CoroutineScheduler *Holder::scheduler()
    {
        return m_scheduler;
    }

    CoroutineBase::wptr Holder::coroutine()
    {
        return m_co;
    }

    int Holder::index()
    {
        return m_index;
    }
}