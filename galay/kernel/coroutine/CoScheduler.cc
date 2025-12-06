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
                if(auto co_ptr = co.lock()){
                    // 检查状态：只有 Waking 状态才能 resume
                    // 如果在队列中等待期间被 destroy，状态会变成 Destroying
                    if (co_ptr->isWaking()) {
                        co_ptr->modToRunning();
                        co_ptr->resume();
                    } else if (co_ptr->isDestroying()) {
                        // 在队列中等待时被标记为销毁，执行销毁而不是恢复
                        LogDebug("CoroutineConsumer: coroutine marked for destroy during resume, executing destroy");
                        co_ptr->destroy();
                    } else {
                        LogDebug("CoroutineConsumer: unexpected status for resume, skip");
                    }
                }
                break;
            case CoroutineActionType::kCoroutineActionTypeDestory:
                if(auto co_ptr = co.lock()) {
                    // 只有 Destroying 状态才能 destroy
                    if (co_ptr->isDestroying()) {
                        co_ptr->destroy();
                    } else {
                        LogDebug("CoroutineConsumer: unexpected status for destroy, skip");
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
        if (auto co_ptr = co.lock()) {
            // 使用原子的 compare_exchange 一次性完成状态检查和修改
            // 只有当状态为 Suspended 时才能成功转换为 Waking
            CoroutineStatus actual_status;
            if (!co_ptr->tryModToWaking(actual_status)) {
                // 转换失败，说明协程不在 Suspended 状态
                switch(actual_status) {
                    case CoroutineStatus::Waking:
                        LogDebug("resumeCoroutine: already Waking, skip");
                        break;
                    case CoroutineStatus::Running:
                        LogDebug("resumeCoroutine: already Running, skip");
                        break;
                    case CoroutineStatus::Finished:
                        LogDebug("resumeCoroutine: already Finished, skip");
                        break;
                    case CoroutineStatus::Destroying:
                        LogDebug("resumeCoroutine: already Destroying, skip");
                        break;
                    default:
                        LogDebug("resumeCoroutine: unexpected status");
                        break;
                }
                return true;  // 这些状态都不需要重新调度
            }
            
            // 成功将状态从 Suspended 转换为 Waking
            co_ptr->belongScheduler(this);
            m_consumer->consume(CoroutineActionType::kCoroutineActionTypeResume, co);
            return true;
        }
        LogWarn("resumeCoroutine: coroutine expired");
        return false;
    }

    bool CoroutineScheduler::destroyCoroutine(CoroutineBase::wptr co)
    {
        if (auto co_ptr = co.lock()) {
            // 使用原子的 compare_exchange 尝试将状态转换为 Destroying
            // 可以从 Suspended 或 Waking 状态转换
            // 这样即使协程已经在调度队列中（Waking状态），也能将其标记为销毁
            CoroutineStatus actual_status;
            if (!co_ptr->tryModToDestroying(actual_status)) {
                // 转换失败，检查实际状态
                switch(actual_status) {
                    case CoroutineStatus::Running:
                        LogWarn("destroyCoroutine: coroutine is Running, cannot destroy now");
                        return false;
                    case CoroutineStatus::Finished:
                        LogDebug("destroyCoroutine: already Finished, skip");
                        return true;
                    case CoroutineStatus::Destroying:
                        LogDebug("destroyCoroutine: already Destroying, skip");
                        return true;
                    default:
                        LogDebug("destroyCoroutine: unexpected status");
                        return false;
                }
            }
            
            // 成功将状态转换为 Destroying，加入销毁队列
            co_ptr->belongScheduler(this);
            m_consumer->consume(CoroutineActionType::kCoroutineActionTypeDestory, co);
            return true;
        }
        return false;
    }


}