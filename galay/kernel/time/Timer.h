#ifndef GALAY_TIMER_H
#define GALAY_TIMER_H 

#include <memory>
#include <atomic>
#include <chrono>
#include "galay/kernel/coroutine/Waker.h"
#include "galay/kernel/event/Event.h"

namespace galay 
{ 

    class Timer
    {
    public:
        using ptr = std::shared_ptr<Timer>;
        using wptr = std::weak_ptr<Timer>;
        Timer(std::chrono::milliseconds ms, Waker waker);
        int64_t getDeadline();
        int64_t getRemainTime();
        void resetWaker(Waker waker);
        bool onReady() const;
        void reset(std::chrono::milliseconds ms);
        void execute();
        bool cancel();
    private:
        void beforeAction();
    private:
        Waker m_waker;
        int64_t m_deadline{ -1 };
        int64_t m_expect_deadline{ -1 };
        std::atomic_bool m_cancel {true};
    };
}




#endif