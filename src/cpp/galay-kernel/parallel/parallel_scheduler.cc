/**
 * @file parallel_scheduler.cc
 * @brief 计算密集型任务调度器实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现单线程 ParallelScheduler，通过阻塞并发队列在专用工作线程上
 * 驱动 CPU 密集型协程。
 */

#include "parallel_scheduler.h"

#include <future>

namespace galay::kernel
{

/**
 * @brief 默认构造函数；初始化延迟到 start() 执行
 */
ParallelScheduler::ParallelScheduler()
{
    m_resumeQueue.close();
}

/**
 * @brief 析构函数，确保调度器在销毁前已停止
 */
ParallelScheduler::~ParallelScheduler()
{
    stop();
}

/**
 * @brief 启动计算工作线程
 *
 * @details 原子地切换到运行状态并创建工作线程，线程在进入主循环前
 * 应用已配置的 CPU 亲和性。若已在运行则不做任何操作。
 */
std::expected<void, IOError> ParallelScheduler::start()
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return {};  // 已经在运行
    }
    if (!m_resumeQueue.reopen()) {
        m_running.store(false, std::memory_order_release);
        return std::unexpected(IOError(kNotReady, 0));
    }

    std::promise<void> thread_ready;
    auto ready = thread_ready.get_future();
    m_thread = std::thread([this, thread_ready = std::move(thread_ready)]() mutable {
        detail::SchedulerThreadScope scheduler_thread_scope;
        m_threadId = std::this_thread::get_id();  // 设置调度器线程ID
        thread_ready.set_value();
        (void)applyConfiguredAffinity();
        workerLoop();
    });
    ready.wait();
    return {};
}

/**
 * @brief 停止计算工作线程
 *
 * @details 先关闭专用恢复接纳，再切换运行状态并等待工作线程结束。
 * 线程在退出前会排空已接纳恢复和普通任务。若已停止则保持接纳关闭。
 */
void ParallelScheduler::stop()
{
    m_resumeQueue.close();
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;  // 已经停止
    }

    // 等待线程结束
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

/**
 * @brief 将计算任务入队，在工作线程上执行
 *
 * @param task  待调度的任务
 * @return true 任务绑定并入队成功；false 任务无效
 */
bool ParallelScheduler::schedule(TaskRef task) noexcept
{
    if (!bindTask(task)) {
        return false;
    }
    return m_queue.enqueue(ParallelTask{std::move(task)});
}

bool ParallelScheduler::scheduleWork(ParallelWorkItem work) noexcept
{
    if (!work.valid() || !m_running.load(std::memory_order_acquire)) {
        return false;
    }
    return m_workQueue.enqueue(std::move(work));
}

bool ParallelScheduler::scheduleResume(TaskRef task) noexcept
{
    if (!bindTask(task)) {
        return false;
    }
    return m_resumeQueue.push(std::move(task));
}

/**
 * @brief 以延后语义将计算任务入队
 *
 * @param task  待调度的任务
 * @return true 任务绑定并入队成功
 * @note 当前实现与 schedule() 相同，保留以作语义区分
 */
bool ParallelScheduler::scheduleDeferred(TaskRef task) noexcept
{
    return schedule(std::move(task));
}

/**
 * @brief 在调用线程上立即恢复任务
 *
 * @param task  待执行的任务
 * @return true 任务绑定并恢复成功；false 绑定失败
 */
bool ParallelScheduler::scheduleImmediately(TaskRef task) noexcept
{
    if (!bindTask(task)) {
        return false;
    }
    resume(task);
    return true;
}

/**
 * @brief 工作线程主循环
 *
 * @details 阻塞在并发队列上等待任务，通过恢复协程处理每个任务。
 * 收到停止信号后排空剩余队列任务后退出。
 */
void ParallelScheduler::workerLoop()
{
    ParallelTask task;
    ParallelWorkItem work;

    while (m_running.load(std::memory_order_acquire)) {
        drainResumeQueue();
        if (m_queue.try_dequeue(task)) {
            Scheduler::resume(task.task);
            continue;
        }
        if (m_workQueue.try_dequeue(work)) {
            work.run(work.context, work.index);
            work.reset();
            continue;
        }
        if (!m_resumeQueue.empty()) {
            continue;
        }
        // 两条队列都为空时短暂阻塞，避免空闲线程持续自旋。
        if (!m_queue.wait_dequeue_timed(task, std::chrono::milliseconds(1))) {
            continue;
        }
        // 执行协程
        Scheduler::resume(task.task);
    }

    // 退出前处理剩余任务
    drainResumeQueue();
    for (;;) {
        if (m_queue.try_dequeue(task)) {
            Scheduler::resume(task.task);
            drainResumeQueue();
            continue;
        }
        if (m_workQueue.try_dequeue(work)) {
            work.run(work.context, work.index);
            work.reset();
            drainResumeQueue();
            continue;
        }
        break;
    }
    while (m_queue.try_dequeue(task)) {
        Scheduler::resume(task.task);
        drainResumeQueue();
    }
    while (m_workQueue.try_dequeue(work)) {
        work.run(work.context, work.index);
        work.reset();
        drainResumeQueue();
    }
    drainResumeQueue();
}

void ParallelScheduler::drainResumeQueue()
{
    TaskState* ready = detail::TaskResumeQueue::reverse(
        m_resumeQueue.takeAll());
    while (ready != nullptr) {
        TaskRef task = detail::TaskResumeQueue::popFront(ready);
        Scheduler::resume(task);
    }
}

} // namespace galay::kernel
