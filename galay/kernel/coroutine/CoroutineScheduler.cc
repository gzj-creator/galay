#include "CoroutineScheduler.hpp"

#include <utility>


namespace galay
{
    CoroutineConsumer::uptr CoroutineConsumer::create()
    {
        return std::make_unique<CoroutineConsumer>();
    }

    CoroutineConsumer::CoroutineConsumer()
    = default;

    void CoroutineConsumer::start()
    {
        m_thread = std::thread([this]()
        {
            run();
        });
    }

    void CoroutineConsumer::consume(CoroutineActionType type, CoroutineBase::wptr co)
    {
        m_queue.enqueue(std::make_pair(type, co));
    }


    void CoroutineConsumer::stop()
    {
        consume(CoroutineActionType::kCoroutineActionTypeNone, {});
        if(m_thread.joinable()) m_thread.join();
    }


    void CoroutineConsumer::run()
    {
        std::pair<CoroutineActionType, CoroutineBase::wptr> task;
        while(true) {
            m_queue.wait_dequeue(task);
            auto [type, co] = task;
            switch (type)
            {
            case CoroutineActionType::kCoroutineActionTypeNone:
                return;
            case CoroutineActionType::kCoroutineActionTypeResume:
                if(!co.expired()){
                    co.lock()->resume();
                }
                break;
            case CoroutineActionType::kCoroutineActionTypeDestory:
                if(!co.expired()) {
                    co.lock()->destroy();
                }
                break;
            default:
                break;
            }
        }
    }

    CoroutineScheduler::CoroutineScheduler(CoroutineConsumer::uptr consumer, TimerManagerType type)
        :m_scheduler(std::make_shared<EventScheduler>()), m_consumer(std::move(consumer))
    {
        m_scheduler->makeTimeEvent(type);
    }

    CoroutineScheduler::CoroutineScheduler(EventScheduler::ptr scheduler, CoroutineConsumer::uptr consumer, TimerManagerType type)
        :m_scheduler(std::move(scheduler)), m_consumer(std::move(consumer))
    {
        m_scheduler->makeTimeEvent(type);
    }

    bool CoroutineScheduler::start()
    {
        m_consumer->start();
        return m_scheduler->start();
    }

    bool CoroutineScheduler::stop()
    {
        m_consumer->stop();
        return m_scheduler->stop();
    }

    bool CoroutineScheduler::notify()
    {
        return m_scheduler->notify();
    }

    bool CoroutineScheduler::isRunning() const
    {
        return m_scheduler->isRunning();
    }


    void CoroutineScheduler::resumeCoroutine(CoroutineBase::wptr co)
    {
        if (!co.expired()) co.lock()->belongScheduler(this);
        m_consumer->consume(CoroutineActionType::kCoroutineActionTypeResume, co);
    }

    void CoroutineScheduler::destroyCoroutine(CoroutineBase::wptr co)
    {
        if (!co.expired()) co.lock()->belongScheduler(this);
        m_consumer->consume(CoroutineActionType::kCoroutineActionTypeDestory, co);
    }

    EventScheduler::ptr CoroutineScheduler::getEventScheduler()
    {
        return m_scheduler;
    }

    CoroutineScheduler::uptr CoroutineSchedulerFactory::create(CoroutineConsumer::uptr consumer, TimerManagerType type)
    {
        return std::make_unique<CoroutineScheduler>(std::move(consumer), type);
    }

    CoroutineScheduler::uptr CoroutineSchedulerFactory::create(EventScheduler::ptr scheduler,CoroutineConsumer::uptr consumer, TimerManagerType type)
    {
        return std::make_unique<CoroutineScheduler>(std::move(scheduler), std::move(consumer), type);
    }


}