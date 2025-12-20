#ifndef GALAY_TIMER_H
#define GALAY_TIMER_H 

#include <functional>
#include <memory>
#include <chrono>
#include <queue>

namespace galay 
{ 
    //一个线程无需考虑线程安全
    class Timer
    {
    public:
        using ptr = std::shared_ptr<Timer>;
        using wptr = std::weak_ptr<Timer>;
        Timer(std::chrono::milliseconds ms, const std::function<void()>& callback);
        int64_t getDeadline();
        int64_t getRemainTime();
        void delay(std::chrono::milliseconds ms);
        void execute();
        void cancel();
    private:
        void beforeAction();
    private:
        bool m_cancel {true};
        int64_t m_deadline{ -1 };
        int64_t m_expect_deadline{ -1 };
        std::function<void()> m_callback;
    };

    class TimerManager {
    public:
        using ptr = std::shared_ptr<TimerManager>;
        //放入timer
        virtual void push(Timer::ptr timer) = 0;
        //处理到时间的timer
        virtual void onTimerTick() = 0;
        //获取最早到达的timer(没有返回nullptr)
        virtual Timer::ptr getEarliestTimer() = 0;
    };

    class PriorityQueueTimerManager: public TimerManager
    {
        class TimerCompare
        {
        public:
            TimerCompare() = default;
            bool operator()(const Timer::ptr &a, const Timer::ptr &b) const;
        };
    public:
        void push(Timer::ptr timer) override;
        void onTimerTick() override;
        Timer::ptr getEarliestTimer() override;
        
    private:
        std::priority_queue<Timer::ptr, std::vector<std::shared_ptr<Timer>>, TimerCompare> m_timers;
    };
}




#endif