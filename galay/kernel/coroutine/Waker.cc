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
        // 直接 lock()，避免 expired() 的额外检查
        auto coroutine = m_coroutine.lock();
        if (!coroutine) {
            // 协程已经被销毁，这是正常情况
            return false;
        }

        auto scheduler = coroutine->belongScheduler();
        return scheduler->resumeCoroutine(m_coroutine);
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
