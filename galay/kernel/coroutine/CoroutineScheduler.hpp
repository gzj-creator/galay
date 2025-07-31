#ifndef GALAY_COROUTINE_SCHEDULER_HPP
#define GALAY_COROUTINE_SCHEDULER_HPP

#include <string>
#include <thread>
#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>
#include "galay/common/Base.h"
#include "galay/kernel/event/EventScheduler.h"
#include "Result.hpp"

namespace galay {


    enum class CoroutineActionType: uint8_t {
        kCoroutineActionTypeNone,
        kCoroutineActionTypeResume,
        kCoroutineActionTypeDestory,
    };

    template<typename Func, typename CoRtn, typename ...Args>
    concept AsyncFuncType = requires(Func func, Args... args) {
        { std::forward<Func>(func)(std::forward<Args>(args)...) }
        -> std::same_as<Coroutine<CoRtn>>;
    };



    //想实现自己的协程调度器，继承CoroutineQueue类，并实现AddCoroutine、RunCoroutine、StopCoroutine三个方法
    //后续添加批量入队接口
    class CoroutineConsumerBase
    {
    public:
        using ptr = std::shared_ptr<CoroutineConsumerBase>;
        using uptr = std::unique_ptr<CoroutineConsumerBase>;
        virtual void start() = 0;
        virtual void consume(CoroutineActionType type, CoroutineBase::wptr co) = 0;
        virtual void stop() = 0;
        virtual ~CoroutineConsumerBase() = default;
    };

    class CoroutineConsumer: public CoroutineConsumerBase
    {
    public:
        using uptr = std::unique_ptr<CoroutineConsumer>;

        static CoroutineConsumer::uptr create();

        CoroutineConsumer();
        void start() override;
        void consume(CoroutineActionType type, CoroutineBase::wptr co) override;
        void stop() override;
    private:
        void Run();
    private:
        std::thread m_thread;
        moodycamel::BlockingConcurrentQueue<std::pair<CoroutineActionType, CoroutineBase::wptr>> m_queue;
    };


    class CoroutineScheduler
    {
    public:
        using ptr = std::shared_ptr<CoroutineScheduler>;
        using uptr = std::unique_ptr<CoroutineScheduler>;


        CoroutineScheduler(CoroutineConsumer::uptr consumer, TimerManagerType type = kTimerManagerTypePriorityQueue);
        CoroutineScheduler(EventScheduler::ptr scheduler, CoroutineConsumer::uptr consumer, TimerManagerType type = kTimerManagerTypePriorityQueue);
        std::string name() { return "CoroutineScheduler"; }
        bool start();
        bool stop();
        bool notify();
        bool isRunning() const;

        template<CoType T>
        void schedule(Coroutine<T>&& co);

        void resumeCoroutine(CoroutineBase::wptr co);
        void destroyCoroutine(CoroutineBase::wptr co);

        EventScheduler::ptr getEventScheduler();

    private:
        EventScheduler::ptr m_scheduler;
        CoroutineConsumer::uptr m_consumer;
    };


    class CoroutineSchedulerFactory
    {
    public:
        static CoroutineScheduler::uptr create(CoroutineConsumer::uptr consumer, TimerManagerType type = kTimerManagerTypePriorityQueue);
        static CoroutineScheduler::uptr create(EventScheduler::ptr scheduler, CoroutineConsumer::uptr consumer, TimerManagerType type = kTimerManagerTypePriorityQueue);
    };

    template <CoType T>
    inline void CoroutineScheduler::schedule(Coroutine<T> &&co)
    {
        co.belongScheduler(this);
        m_consumer->consume(CoroutineActionType::kCoroutineActionTypeResume, co.getOriginCoroutine());
    }



}


#endif