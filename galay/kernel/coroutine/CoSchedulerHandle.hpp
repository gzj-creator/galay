#ifndef GALAY_COSCHEDULER_HANDLE_HPP
#define GALAY_COSCHEDULER_HANDLE_HPP

#include "CoScheduler.hpp"

namespace galay 
{
    /**
     * @brief 协程调度器句柄
     * @details 标识某个特定的 CoroutineScheduler 线程
     *          用于将相关协程调度到同一个调度器线程上执行
     */
    class CoSchedulerHandle
    {
    public:
        CoSchedulerHandle() : m_scheduler(nullptr) {}
        CoSchedulerHandle(CoroutineScheduler* scheduler) : m_scheduler(scheduler) {}

        template<CoType T>
        void destory(Coroutine<T>&& co);
        template<CoType T>
        void spawn(Coroutine<T>&& co);
    private:
        CoroutineScheduler* m_scheduler = nullptr;
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