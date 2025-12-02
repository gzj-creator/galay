#include "Waker.h"
#include "CoScheduler.hpp"

namespace galay
{
    Waker::Waker(CoroutineBase::wptr coroutine)
        :m_coroutine(coroutine)
    {
    }

    bool Waker::wakeUp()
    {
        if (!m_coroutine.expired())
        {
            auto coroutine = m_coroutine.lock();
            if (!coroutine) {
                LogWarn("Waker::wakeUp - coroutine lock failed");
                return false;
            }

            auto scheduler = coroutine->belongScheduler();

            return scheduler->resumeCoroutine(m_coroutine);
        } 
        LogWarn("Waker::wakeUp - coroutine expired");
        return false;
    }

    CoroutineBase::wptr Waker::getCoroutine()
    {
        return m_coroutine;
    }

    CoroutineScheduler* Waker::belongScheduler() const
    {
        return m_coroutine.lock()->belongScheduler();
    }

}
