#include "Waker.h"
#include "CoroutineScheduler.hpp"

namespace galay
{
    Waker::Waker(CoroutineBase::wptr coroutine)
        :m_coroutine(coroutine)
    {
    }

    void Waker::wakeUp()
    {
        auto coroutine = m_coroutine.lock();
        if (coroutine)
        {
            if (coroutine->belongScheduler() == nullptr)
            {
                throw std::runtime_error("coroutine is not running on any scheduler");
            }
            coroutine->belongScheduler()->resumeCoroutine(m_coroutine);
        }
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
