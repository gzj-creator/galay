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


    namespace {
        // 处理单个任务的辅助函数
        inline bool processTask(const std::pair<CoroutineActionType, CoroutineBase::wptr>& task) {
            auto [type, co] = task;
            switch (type)
            {
            case CoroutineActionType::kCoroutineActionTypeNone:
                return false;  // 返回 false 表示需要退出
            case CoroutineActionType::kCoroutineActionTypeResume:
                if(auto co_ptr = co.lock()){
                    // 检查状态：只有 Waking 状态才能 resume
                    // 如果在队列中等待期间被 destroy，状态会变成 Destroying
                    if (co_ptr->isWaking()) {
                        co_ptr->modToRunning();
                        co_ptr->resume();
                    } else if (co_ptr->isDestroying()) {
                        // 在队列中等待时被标记为销毁，执行销毁而不是恢复
                        co_ptr->destroy();
                    }
                }
                break;
            case CoroutineActionType::kCoroutineActionTypeDestory:
                if(auto co_ptr = co.lock()) {
                    // 只有 Destroying 状态才能 destroy
                    if (co_ptr->isDestroying()) {
                        co_ptr->destroy();
                    }
                    // 其他状态无需处理，跳过日志
                }
                break;
            default:
                break;
            }
            return true;  // 返回 true 表示继续处理
        }
    }

    void CoroutineConsumer::run()
    {
        std::pair<CoroutineActionType, CoroutineBase::wptr> tasks[CoroutineConsumer::BATCH_SIZE] = {};
        while(true) {
            // 混合策略：先尝试非阻塞批量取出（高吞吐）
            size_t count = m_queue.try_dequeue_bulk(tasks, CoroutineConsumer::BATCH_SIZE);
            
            if (count > 0) {
                // 批量处理：高负载时提高吞吐量
                for(size_t i = 0; i < count; ++i) {
                    if (!processTask(tasks[i])) {
                        return;  // 收到停止信号
                    }
                }
            } else {
                // 单处理：低负载时降低延迟
                std::pair<CoroutineActionType, CoroutineBase::wptr> task;
                m_queue.wait_dequeue(task);
                if (!processTask(task)) {
                    return;  // 收到停止信号
                }
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
                // 这些状态都不需要重新调度，无需日志输出
                return true;
            }

            // 成功将状态从 Suspended 转换为 Waking
            co_ptr->belongScheduler(this);
            m_consumer->consume(CoroutineActionType::kCoroutineActionTypeResume, co);
            return true;
        }
        return false;
    }

    bool CoroutineScheduler::destroyCoroutine(CoroutineBase::wptr co)
    {
        if (auto co_ptr = co.lock()) {
            // 使用原子的 compare_exchange 尝试将状态转换为 Destroying
            // 可以从 Suspended 或 Waking 状态转换
            CoroutineStatus actual_status;
            if (!co_ptr->tryModToDestroying(actual_status)) {
                // 转换失败，检查实际状态
                if (actual_status == CoroutineStatus::Running) {
                    // Running 状态不能销毁（保留此日志用于调试）
                    LogWarn("destroyCoroutine: coroutine is Running, cannot destroy now");
                    return false;
                }
                // Finished/Destroying 状态返回 true（已完成或已在销毁中）
                return actual_status == CoroutineStatus::Finished ||
                       actual_status == CoroutineStatus::Destroying;
            }

            // 成功将状态转换为 Destroying，加入销毁队列
            co_ptr->belongScheduler(this);
            m_consumer->consume(CoroutineActionType::kCoroutineActionTypeDestory, co);
            return true;
        }
        return false;
    }


}