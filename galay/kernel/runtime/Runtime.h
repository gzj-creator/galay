//
// Created by gong on 2025/6/28.
//

#ifndef GALAY_RUNTIME_H
#define GALAY_RUNTIME_H

#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/time/TimerManager.h"
#include <optional>
#include <vector>

namespace galay
{

#define DEFAULT_FDS_SET_INITIAL_SIZE        1024
#define DEFAULT_COS_SCHEDULER_THREAD_NUM    4

    class AsyncFactory;
    //可容忍在改变状态之前读取旧值导致老队列管理co（下一轮也可以检查到）
    class CoroutineManager
    {
    public:
        using uptr = std::unique_ptr<CoroutineManager>;
        CoroutineManager(std::chrono::milliseconds interval);
        CoroutineManager(CoroutineManager&& cm);
        CoroutineManager& operator=(CoroutineManager&& cm);
        void start();
        void manage(CoroutineBase::wptr co);
        void stop();
    private:
        void run();
        void autoCheck();
    private:
        std::chrono::milliseconds m_interval;
        std::thread m_thread;
        std::atomic_bool m_running = false;
        std::atomic_bool m_change = false;
        moodycamel::ConcurrentQueue<CoroutineBase::wptr> m_queue_1;
        moodycamel::ConcurrentQueue<CoroutineBase::wptr> m_queue_2;
    };

    class Runtime;

    class RuntimeVisitor {
    public:
        RuntimeVisitor(Runtime& runtime);
        EventScheduler::ptr eventScheduler();
        TimerManager::ptr timerManager();
        int& eventCheckTimeout();
        std::atomic_int32_t& index();
        CoroutineManager::uptr& coManager();
        std::vector<CoroutineScheduler>& coScheduler();
    private:
        Runtime& m_runtime;
    };

    class Runtime
    {
        friend class RuntimeBuilder;
        friend class RuntimeVisitor;
    public:

        using Token = int;
        Runtime();
        Runtime(
            int eTimeout,
            int coSchedulerNum,
            EventScheduler::ptr eScheduler,
            TimerManager::ptr timerManager,
            std::chrono::milliseconds coManagerInterval = std::chrono::milliseconds(-1)
        );
        Runtime(Runtime&& rt);
        Runtime& operator=(Runtime&& rt);


        AsyncFactory getAsyncFactory();
        std::optional<CoSchedulerHandle> getCoSchedulerHandle(Token token);

        void startCoManager(std::chrono::milliseconds interval);
        void start();
        void stop();
        bool isRunning();
        size_t coSchedulerSize();
        //thread security
        // return co_id
        template<CoType T>
        CoSchedulerHandle schedule(Coroutine<T>&& co);
        // return co_id
        template<CoType T>
        CoSchedulerHandle schedule(Coroutine<T>&& co, Token token);

        CoSchedulerHandle schedule(CoroutineBase::wptr co);
        CoSchedulerHandle schedule(CoroutineBase::wptr co, Token token);
    private:
        int m_eTimeout = -1;
        std::atomic_bool m_running = false;
        std::atomic_int32_t m_index = 0;
        EventScheduler::ptr m_eScheduler;
        TimerManager::ptr m_timerManager;
        CoroutineManager::uptr m_cManager;
        std::vector<CoroutineScheduler> m_cSchedulers;
    };

    class RuntimeBuilder {
    public:
        // == 0 means not use coManager, num is associated with coScheduler
        RuntimeBuilder& startCoManager(std::chrono::milliseconds interval);
        RuntimeBuilder& setEventCheckTimeout(int timeout);
        RuntimeBuilder& setCoSchedulerNum(int num);
        RuntimeBuilder& setEventSchedulerInitFdsSize(int fds_set_size);
        RuntimeBuilder& useExternalEventScheduler(EventScheduler::ptr scheduler);
        Runtime build();
    private:
        int m_eTimeout = -1;
        int m_coSchedulerNum = DEFAULT_COS_SCHEDULER_THREAD_NUM;
        std::chrono::milliseconds m_interval = std::chrono::milliseconds(-1);
        int m_eventSchedulerInitFdsSize = DEFAULT_FDS_SET_INITIAL_SIZE;
        EventScheduler::ptr m_eventScheduler;
    };

    template<CoType T>
    inline CoSchedulerHandle Runtime::schedule(Coroutine<T>&& co)
    {
        return schedule(co.origin());
    }

    template<CoType T>
    inline CoSchedulerHandle Runtime::schedule(Coroutine<T>&& co, Token token)
    {
        return schedule(co.origin(), token);
    }

}



#endif //GALAY_RUNTIME_H