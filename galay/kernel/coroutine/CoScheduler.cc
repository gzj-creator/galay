#include "CoScheduler.hpp"
#include "galay/common/Common.h"
#include "kernel/coroutine/Coroutine.hpp"
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
                    auto co_ptr = co.lock();
                    if(!co_ptr) {
                        LogWarn("Consumer: coroutine lock failed in Resume action");
                        break;
                    }
                    // 正常情况：Scheduled → Running
                    co_ptr->modToRunning();
                    co_ptr->resume();
                }
                break;
            case CoroutineActionType::kCoroutineActionTypeDestory:
                if(!co.expired()) {
                    auto co_ptr = co.lock();
                    if(co_ptr) {
                        co_ptr->destroy();
                    }
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

    bool CoroutineScheduler::schedule(CoroutineBase::wptr co)
    {
        return resumeCoroutine(co);
    }

    bool CoroutineScheduler::resumeCoroutine(CoroutineBase::wptr co)
    {
        if (!co.expired()) {
            auto co_ptr = co.lock();
            if(!co_ptr) {
                LogWarn("resumeCoroutine: coroutine lock failed");
                return false;
            }

            // 检查当前状态
            auto status_str = co_ptr->isRunning() ? "Running" :
                             co_ptr->isSuspend() ? "Suspended" :
                             co_ptr->isScheduled() ? "Scheduled" :
                             co_ptr->isDone() ? "Finished" : "Unknown";
            LogDebug("resumeCoroutine: current status = {}", status_str);

            // 如果已经是Scheduled状态，说明已经在队列中了，直接返回成功
            if(co_ptr->isScheduled()) {
                LogDebug("resumeCoroutine: already Scheduled, skip");
                return true;
            }

            // 如果已经完成，不需要恢复
            if(co_ptr->isDone()) {
                LogDebug("resumeCoroutine: already Finished, skip");
                return true;
            }

            // 运行中，不需要恢复
            if(co_ptr->isRunning()) {
                LogDebug("resumeCoroutine: already Running, skip");
                return true;
            }

            co_ptr->modToScheduled();
            co_ptr->belongScheduler(this);
            m_consumer->consume(CoroutineActionType::kCoroutineActionTypeResume, co);
            return true;
        }
        LogWarn("resumeCoroutine: coroutine expired");
        return false;
    }

    bool CoroutineScheduler::destroyCoroutine(CoroutineBase::wptr co)
    {
        if (!co.expired()) {
            auto co_ptr = co.lock();
            if(!co_ptr) {
                LogWarn("destroyCoroutine: coroutine lock failed");
                return false;
            }
            co_ptr->belongScheduler(this);
            m_consumer->consume(CoroutineActionType::kCoroutineActionTypeDestory, co);
            return true;
        }
        return false;
    }


}