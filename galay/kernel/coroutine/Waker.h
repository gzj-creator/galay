#ifndef GALAY_WAKER_H
#define GALAY_WAKER_H

#include "Coroutine.hpp"

namespace galay
{

    //must running on scheduler
    class Waker
    {
    public:
        Waker() {}
        Waker(CoroutineBase::wptr coroutine);
        bool wakeUp();

        CoroutineBase::wptr getCoroutine();
        CoroutineScheduler* belongScheduler() const;
    private:
        CoroutineBase::wptr m_coroutine;
    };
}



#endif
