//
// Created by gong on 2025/6/28.
//

#ifndef GALAY_RUNTIME_H
#define GALAY_RUNTIME_H

#include "galay/kernel/coroutine/CoroutineScheduler.hpp"
#include <unordered_set>

namespace galay
{
    //可容忍在改变状态之前读取旧值导致老队列管理co（下一轮也可以检查到）
    class CoroutineManager
    {
    public:
        using uptr = std::unique_ptr<CoroutineManager>;
        CoroutineManager(CoroutineScheduler* scheduler, std::chrono::milliseconds interval);
        void start();
        void manage(CoroutineBase::wptr co);
        void stop();
    private:
        void run();
        void autoCheck();
    private:
        CoroutineScheduler* m_scheduler;
        std::chrono::milliseconds m_interval;
        std::thread m_thread;
        std::atomic_bool m_running = false;
        std::atomic_bool m_change = false;
        moodycamel::ConcurrentQueue<CoroutineBase::wptr> m_queue_1;
        moodycamel::ConcurrentQueue<CoroutineBase::wptr> m_queue_2;
    };

    class Runtime
    {
    public:
        using uptr = std::unique_ptr<Runtime>;

        Runtime(bool start_check = false, std::chrono::milliseconds check_interval = std::chrono::milliseconds(800),\
            TimerManagerType type = TimerManagerType::kTimerManagerTypePriorityQueue);
        //thread security
        template<CoType T>
        void schedule(Coroutine<T>&& co);
        ~Runtime();
    private:
        CoroutineManager::uptr m_manager;
        CoroutineScheduler::uptr m_scheduler;
    };

    template<CoType T>
    inline void Runtime::schedule(Coroutine<T>&& co)
    {
        if (m_manager)
        {
            m_manager->manage(co.getOriginCoroutine());
        }
        m_scheduler->schedule(std::forward<Coroutine<T>>(co));
    }

}



#endif //GALAY_RUNTIME_H