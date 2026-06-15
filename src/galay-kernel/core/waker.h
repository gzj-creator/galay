/**
 * @file waker.h
 * @brief Coroutine wake-up handle
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details Defines Waker, a lightweight handle that holds a TaskRef and can
 * request the task's scheduler to resume the associated coroutine. Used by
 * IO completion callbacks, timeouts, and other async events to re-schedule
 * a suspended coroutine.
 */

#ifndef GALAY_KERNEL_WAKER_H
#define GALAY_KERNEL_WAKER_H

#include <concepts>
#include "task.h"

namespace galay::kernel
{

/**
 * @brief 任务唤醒器
 * @details 持有 `TaskRef`，可在其他线程或回调中把关联协程重新交回所属调度器。
 */
class Waker
{
public:
    Waker() = default;  ///< 构造空 waker
    explicit Waker(TaskRef task) noexcept;  ///< 以任务引用构造唤醒器
    template <typename Promise>
    requires requires(const Promise& promise) {
        { promise.taskRefView() } -> std::same_as<const TaskRef&>;
    }
    explicit Waker(std::coroutine_handle<Promise> handle) noexcept
        : m_task(handle.promise().taskRefView())
    {
    }
    Waker(const Waker& other) = default;  ///< 拷贝唤醒器，底层共享同一任务引用
    Waker(Waker&& waker) noexcept = default;  ///< 移动唤醒器
    Waker& operator=(const Waker& other) = default;  ///< 拷贝赋值唤醒器
    Waker& operator=(Waker&& other) noexcept = default;  ///< 移动赋值唤醒器

    Scheduler* getScheduler();  ///< 返回关联任务的所属调度器；无任务时返回 nullptr

    void wakeUp();  ///< 请求恢复关联任务；若任务无效或调度器不可用则静默忽略

private:
    TaskRef m_task;  ///< 被唤醒的目标任务
};



} // namespace galay::kernel

#endif // GALAY_KERNEL_WAKER_H
