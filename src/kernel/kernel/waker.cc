/**
 * @file waker.cc
 * @brief Waker 实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现从 TaskRef 构造 Waker、调度器查找以及重新调度目标协程的唤醒请求。
 */

#include "waker.h"
#include "scheduler.hpp"

namespace galay::kernel {


/**
 * @brief 构造持有给定任务引用的 Waker
 *
 * @param task  此 Waker 将恢复的协程任务
 */
Waker::Waker(TaskRef task) noexcept
    : m_task(std::move(task))
{
}

/**
 * @brief 查找持有任务关联的调度器
 *
 * @return 所属 Scheduler 指针，若任务未绑定则返回 nullptr
 */
Scheduler* Waker::getScheduler()
{
    return m_task.belongScheduler();
}

/**
 * @brief 请求在所属调度器上恢复持有任务
 *
 * @details 调用 detail::requestTaskResume，原子地将任务标记为已入队
 * 并提交到调度器。若任务已入队、无效或已完成，请求被静默忽略。
 */
void Waker::wakeUp()
{
    detail::requestTaskResume(m_task);
}

}
