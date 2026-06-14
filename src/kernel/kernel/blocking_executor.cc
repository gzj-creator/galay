/**
 * @file blocking_executor.cc
 * @brief 自适应阻塞任务执行器实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现动态伸缩的 BlockingExecutor 线程池。
 * 工作线程按需创建，遵循可配置的保活超时，空闲时自动收缩到最小线程数。
 */

#include "blocking_executor.h"
#include <thread>
#include <utility>

namespace galay::kernel
{

namespace
{

/// 多余线程退出前的默认空闲超时时间
constexpr auto kDefaultBlockingKeepAlive = std::chrono::milliseconds(5000);

} // namespace

/**
 * @brief 使用默认线程数和保活超时构造
 */
BlockingExecutor::BlockingExecutor()
    : BlockingExecutor(0, defaultMaxWorkers(), kDefaultBlockingKeepAlive)
{
}

/**
 * @brief 使用自定义线程数和保活超时构造
 *
 * @param minWorkers  最少保留的工作线程数
 * @param maxWorkers  允许的最大工作线程数
 * @param keepAlive   多余线程的空闲超时时间
 */
BlockingExecutor::BlockingExecutor(size_t minWorkers,
                                   size_t maxWorkers,
                                   std::chrono::milliseconds keepAlive)
    : m_minWorkers(minWorkers),
      m_maxWorkers(maxWorkers > 0 ? maxWorkers : 1),
      m_keepAlive(keepAlive)
{
    if (m_minWorkers > m_maxWorkers) {
        m_minWorkers = m_maxWorkers;
    }
}

/**
 * @brief 析构函数，通知停止并等待所有工作线程退出
 */
BlockingExecutor::~BlockingExecutor()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_stopping = true;
    m_taskCv.notify_all();
    m_shutdownCv.wait(lock, [this]() { return m_workerCount == 0; });
}

/**
 * @brief 根据硬件并发度推导默认最大工作线程数
 *
 * @return 可用 CPU 核心数，无法确定时返回 4
 */
size_t BlockingExecutor::defaultMaxWorkers()
{
    const size_t hardware = std::thread::hardware_concurrency();
    return hardware > 0 ? hardware : 4;
}

/**
 * @brief 提交一个阻塞任务
 *
 * @details 若存在空闲工作线程，任务入队并唤醒一个线程；
 * 若线程池未达到上限，则创建新的分离线程并以该任务作为初始工作；
 * 否则任务入队等待工作线程可用时处理。
 *
 * @param task  待执行的非空调用对象
 *
 * @return 成功时返回空 expected；执行器停止时返回 BlockingExecutorError
 */
std::expected<void, BlockingExecutorError> BlockingExecutor::submit(std::function<void()> task)
{
    if (!task) {
        return {};
    }

    bool shouldNotify = false;
    bool shouldSpawn = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return std::unexpected(BlockingExecutorError(BlockingExecutorErrorCode::kStopping));
        }

        if (m_idleWorkers > 0) {
            m_tasks.push_back(std::move(task));
            shouldNotify = true;
        } else if (m_workerCount < m_maxWorkers) {
            ++m_workerCount;
            shouldSpawn = true;
        } else {
            m_tasks.push_back(std::move(task));
        }
    }

    if (shouldNotify) {
        m_taskCv.notify_one();
        return {};
    }

    if (!shouldSpawn) {
        return {};
    }

    std::thread([this, initialTask = std::move(task)]() mutable {
        workerLoop(std::move(initialTask));
    }).detach();
    return {};
}

/**
 * @brief 工作线程主循环
 *
 * @details 执行初始任务后进入循环等待新任务。
 * 超过最小线程数的空闲线程在保活超时后退出。
 * 关闭时排空剩余任务后退出。
 *
 * @param initialTask  首个执行的任务（来自创建该线程的 submit() 调用）
 */
void BlockingExecutor::workerLoop(std::function<void()> initialTask)
{
    std::function<void()> task = std::move(initialTask);

    for (;;) {
        if (task) {
            task();
            task = {};
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_tasks.empty() && !m_stopping) {
            ++m_idleWorkers;
            const bool canTimeOut = m_workerCount > m_minWorkers;

            bool ready = true;
            if (canTimeOut) {
                ready = m_taskCv.wait_for(lock, m_keepAlive, [this]() {
                    return m_stopping || !m_tasks.empty();
                });
            } else {
                m_taskCv.wait(lock, [this]() {
                    return m_stopping || !m_tasks.empty();
                });
            }

            --m_idleWorkers;

            if (!ready && m_tasks.empty() && m_workerCount > m_minWorkers) {
                retireWorkerLocked();
                return;
            }
        }

        if (m_stopping && m_tasks.empty()) {
            retireWorkerLocked();
            return;
        }

        if (m_tasks.empty()) {
            continue;
        }

        task = std::move(m_tasks.front());
        m_tasks.pop_front();
    }
}

/**
 * @brief 在持锁状态下递减工作线程计数，必要时通知关闭等待
 *
 * @note 必须在持有 m_mutex 时调用
 */
void BlockingExecutor::retireWorkerLocked()
{
    --m_workerCount;
    if (m_stopping && m_workerCount == 0) {
        m_shutdownCv.notify_all();
    }
}

} // namespace galay::kernel
