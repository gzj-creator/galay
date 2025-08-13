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
        friend void bind(Timer::ptr timer, details::Event* event);
    
        Timer(std::chrono::milliseconds ms, const std::function<void()>& callback);
        int64_t getDeadline() const;
        int64_t getRemainTime() const;
        void execute();
        bool cancel();
    private:
        std::function<void()> m_callback;
        int64_t m_deadline{ -1 };
        std::atomic_bool m_cancel {true};
    };
}




#endif