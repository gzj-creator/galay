#ifndef GALAY_TIMER_H
#define GALAY_TIMER_H 

#include <memory>
#include <atomic>
#include <chrono>
#include "galay/kernel/coroutine/Waker.h"

namespace galay 
{ 
    class Timer: public std::enable_shared_from_this<Timer> 
    {
    public:
        using ptr = std::shared_ptr<Timer>;
        using wptr = std::weak_ptr<Timer>;
    
        Timer(std::chrono::milliseconds ms, Waker waker);
        int64_t getDeadline() const;
        int64_t getRemainTime() const;
        void execute();
        bool cancel();
    private:
        Waker m_waker;
        int64_t m_deadline{ -1 };
        std::atomic_bool m_cancel {true};
    };

}




#endif