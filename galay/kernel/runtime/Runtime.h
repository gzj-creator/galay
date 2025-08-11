//
// Created by gong on 2025/6/28.
//

#ifndef GALAY_RUNTIME_H
#define GALAY_RUNTIME_H

#include "galay/kernel/coroutine/CoScheduler.hpp"
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

    class Runtime;

    class RuntimeConfig {
    public:
        RuntimeConfig(Runtime& runtime);
        RuntimeConfig& eventTimeout(int64_t timeout);
        RuntimeConfig& startCoManager(bool start);
    private:
        Runtime& m_runtime;
    };

    class Runtime
    {
        friend class RuntimeConfig;
    public:
        using uptr = std::unique_ptr<Runtime>;
        RuntimeConfig config();
        void start();
        void stop();
        //thread security
        template<CoType T>
        void schedule(Coroutine<T>&& co);

        EventScheduler* eventScheduler() { return m_eScheduler.get(); }
        CoroutineScheduler* coroutineScheduler() { return m_cScheduler.get(); }

        ~Runtime();
    private:
        int m_event_timeout = -1;

        bool m_start_check_co = false;
        std::chrono::milliseconds m_co_check_interval;
        EventScheduler::ptr m_eScheduler;
        CoroutineManager::uptr m_manager;
        CoroutineScheduler::uptr m_cScheduler;
    };

    template<CoType T>
    inline void Runtime::schedule(Coroutine<T>&& co)
    {
        if (m_manager)
        {
            m_manager->manage(co.getOriginCoroutine());
        }
        if(!m_eScheduler || !m_cScheduler) {
            throw std::runtime_error("Runtime not started");
        }
        m_cScheduler->schedule(std::forward<Coroutine<T>>(co));
    }

}



#endif //GALAY_RUNTIME_H