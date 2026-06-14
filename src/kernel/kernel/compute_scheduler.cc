/**
 * @file compute_scheduler.cc
 * @brief 计算密集型任务调度器实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现单线程 ComputeScheduler，通过阻塞并发队列在专用工作线程上
 * 驱动 CPU 密集型协程。
 */

#include "compute_scheduler.h"

namespace galay::kernel
{

/**
 * @brief 默认构造函数；初始化延迟到 start() 执行
 */
ComputeScheduler::ComputeScheduler()
{
}

/**
 * @brief 析构函数，确保调度器在销毁前已停止
 */
ComputeScheduler::~ComputeScheduler()
{
    stop();
}

/**
 * @brief 启动计算工作线程
 *
 * @details 原子地切换到运行状态并创建工作线程，线程在进入主循环前
 * 应用已配置的 CPU 亲和性。若已在运行则不做任何操作。
 */
void ComputeScheduler::start()
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // 已经在运行
    }

    m_thread = std::thread([this]() {
        m_threadId = std::this_thread::get_id();  // 设置调度器线程ID
        (void)applyConfiguredAffinity();
        workerLoop();
    });
}

/**
 * @brief 停止计算工作线程
 *
 * @details 将停止信号入队并等待工作线程结束。
 * 线程在退出前会排空剩余任务。若已停止则不做任何操作。
 */
void ComputeScheduler::stop()
{
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;  // 已经停止
    }

    // 发送停止信号唤醒等待的线程
    m_queue.enqueue(ComputeTask{TaskRef{}, true});

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
bool ComputeScheduler::schedule(TaskRef task)
{
    if (!bindTask(task)) {
        return false;
    }
    m_queue.enqueue(ComputeTask{std::move(task)});
    return true;
}

/**
 * @brief 以延后语义将计算任务入队
 *
 * @param task  待调度的任务
 * @return true 任务绑定并入队成功
 * @note 当前实现与 schedule() 相同，保留以作语义区分
 */
bool ComputeScheduler::scheduleDeferred(TaskRef task)
{
    return schedule(std::move(task));
}

/**
 * @brief 在调用线程上立即恢复任务
 *
 * @param task  待执行的任务
 * @return true 任务绑定并恢复成功；false 绑定失败
 */
bool ComputeScheduler::scheduleImmediately(TaskRef task)
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
void ComputeScheduler::workerLoop()
{
    ComputeTask task;

    while (true) {
        // 阻塞等待任务（无超时，由任务驱动）
        if (!m_queue.wait_dequeue_timed(task, std::chrono::milliseconds(1))) {
            continue;
        }
        // 停止信号
        if (task.is_stop_signal) {
            break;
        }
        // 执行协程
        Scheduler::resume(task.task);
    }

    // 退出前处理剩余任务
    while (m_queue.try_dequeue(task)) {
        if (task.is_stop_signal) {
            continue;
        }
        Scheduler::resume(task.task);
    }
}

} // namespace galay::kernel
