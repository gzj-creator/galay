#ifndef GALAY_CO_SCHEDULER_HPP
#define GALAY_CO_SCHEDULER_HPP

#include <string>
#include <thread>
#include "galay/common/Base.h"
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
        static constexpr size_t BATCH_SIZE = 1024;

        using uptr = std::unique_ptr<CoroutineConsumer>;

        static CoroutineConsumer::uptr create();

        CoroutineConsumer();
        void start() override;
        void consume(CoroutineActionType type, CoroutineBase::wptr co) override;
        void stop() override;
    private:
        void run();
    private:
        std::thread m_thread;
        moodycamel::BlockingConcurrentQueue<std::pair<CoroutineActionType, CoroutineBase::wptr>> m_queue;
    };


    class CoroutineScheduler
    {
    public:
        using ptr = std::shared_ptr<CoroutineScheduler>;
        using uptr = std::unique_ptr<CoroutineScheduler>;


        CoroutineScheduler(CoroutineConsumer::uptr consumer);
        std::string name() { return "CoroutineScheduler"; }
        bool start();
        bool stop();
        template<CoType T>
        bool schedule(Coroutine<T>&& co);

        bool schedule(CoroutineBase::wptr co);

        bool resumeCoroutine(CoroutineBase::wptr co);
        bool destroyCoroutine(CoroutineBase::wptr co);

    private:
        CoroutineConsumer::uptr m_consumer;
    };

    template <CoType T>
    inline bool CoroutineScheduler::schedule(Coroutine<T> &&co)
    {
        resumeCoroutine(co.origin());
    }



}


#endif