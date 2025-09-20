#include "CoScheduler.hpp"
#include "galay/common/Common.h"
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
        m_thread = std::thread([this]() {
            setThreadName("CoroutineConsumer");
            run();
            LogTrace("CoroutineConsumer exit successfully!");
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

    CoroutineScheduler::CoroutineScheduler(CoroutineConsumer::uptr consumer)
        :m_consumer(std::move(consumer))
    {
    }

    bool CoroutineScheduler::start()
    {
        m_consumer->start();
        return true;
    }

    bool CoroutineScheduler::stop()
    {
        m_consumer->stop();
        return true;
    }

    void CoroutineScheduler::schedule(CoroutineBase::wptr co)
    {
        if(!co.expired()) {
            co.lock()->belongScheduler(this);
            m_consumer->consume(CoroutineActionType::kCoroutineActionTypeResume, co);
        }
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


}