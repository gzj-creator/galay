#ifndef GALAY_CO_SCHEDULER_HPP
#define GALAY_CO_SCHEDULER_HPP

#include <memory>
#include <string>
#include <thread>
#include "galay/common/Base.h"

namespace galay {


    enum class CoroutineActionType: uint8_t {
        kCoroutineActionTypeNone,
        kCoroutineActionTypeResume,
        kCoroutineActionTypeDestory,
    };

    class CoroutineBase;

    //想实现自己的协程调度器，继承CoroutineQueue类，并实现AddCoroutine、RunCoroutine、StopCoroutine三个方法
    //后续添加批量入队接口
    class CoroutineConsumerBase
    {
    public:
        using ptr = std::shared_ptr<CoroutineConsumerBase>;
        using uptr = std::unique_ptr<CoroutineConsumerBase>;
        virtual void start() = 0;
        virtual void consume(CoroutineActionType type, std::weak_ptr<CoroutineBase> co) = 0;
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
        void consume(CoroutineActionType type, std::weak_ptr<CoroutineBase> co) override;
        void stop() override;
    private:
        void run();
    private:
        std::thread m_thread;
        moodycamel::BlockingConcurrentQueue<std::pair<CoroutineActionType, std::weak_ptr<CoroutineBase>>> m_queue;
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

        bool resumeCoroutine(std::weak_ptr<CoroutineBase> co);
        bool destroyCoroutine(std::weak_ptr<CoroutineBase> co);

    private:
        CoroutineConsumer::uptr m_consumer;
    };


}


#endif