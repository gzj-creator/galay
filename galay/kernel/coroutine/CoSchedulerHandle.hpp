#ifndef GALAY_COSCHEDULER_HANDLE_HPP
#define GALAY_COSCHEDULER_HANDLE_HPP

#include "Coroutine.hpp"

namespace galay
{
    class Runtime;
    class EventScheduler;
    class TimerManager;
    class AsyncFactory;

    /**
     * @brief 协程调度器句柄
     * @details 标识某个特定的 CoroutineScheduler 线程，并提供访问 Runtime 全局资源的能力
     *          用于将相关协程调度到同一个调度器线程上执行
     */
    class CoSchedulerHandle
    {
    public:
        CoSchedulerHandle() : m_scheduler(nullptr), m_runtime(nullptr) {}
        CoSchedulerHandle(CoroutineScheduler* scheduler, Runtime* runtime)
            : m_scheduler(scheduler), m_runtime(runtime) {}

        template<CoType T>
        void destory(Coroutine<T>&& co);
        template<CoType T>
        void spawn(Coroutine<T>&& co);

        void destory(CoroutineBase::wptr co);
        void spawn(CoroutineBase::wptr co);

        CoroutineScheduler* scheduler() const { return m_scheduler; }
        Runtime* runtime() const { return m_runtime; }
        EventScheduler* eventScheduler() const;
        TimerManager* timerManager() const;
        AsyncFactory getAsyncFactory() const;
    private:
        CoroutineScheduler* m_scheduler = nullptr;
        Runtime* m_runtime = nullptr;
    };


    template<CoType T>
    void CoSchedulerHandle::destory(Coroutine<T>&& co)
    {
        m_scheduler->destroyCoroutine(co.origin());
    }

    template<CoType T>
    void CoSchedulerHandle::spawn(Coroutine<T>&& co)
    {
        m_scheduler->resumeCoroutine(co.origin());
    }
}


#endif