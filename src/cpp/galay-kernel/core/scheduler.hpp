/**
 * @file scheduler.hpp
 * @brief 协程调度器基类与任务调度辅助函数
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义抽象 Scheduler 接口（start、stop、schedule、scheduleDeferred、
 * scheduleImmediately），包含处理 Runtime 作用域的共享 resume() 实现。
 * 同时提供用于便捷 Task 提交的 scheduleTask 重载函数。
 *
 * @note 具体实现：KqueueScheduler (macOS)、EpollScheduler (Linux)、
 *       IOUringScheduler (Linux)、ComputeScheduler
 */

#ifndef GALAY_KERNEL_SCHEDULER_HPP
#define GALAY_KERNEL_SCHEDULER_HPP

#include "../common/timer.hpp"
#include "../common/error.h"
#include "task.h"
#include <atomic>
#include <cstdint>
#include <expected>
#include <optional>
#include <thread>

#ifdef USE_IOURING
#include <linux/time_types.h>
#endif

/**
 * @def GALAY_SCHEDULER_MAX_EVENTS
 * @brief 调度器单次处理的最大事件数
 * @note 可在编译时通过 -DGALAY_SCHEDULER_MAX_EVENTS=xxx 覆盖
 */
#ifndef GALAY_SCHEDULER_MAX_EVENTS
#define GALAY_SCHEDULER_MAX_EVENTS 1024
#endif

/**
 * @def GALAY_SCHEDULER_BATCH_SIZE
 * @brief 协程批量处理大小
 * @note 可在编译时通过 -DGALAY_SCHEDULER_BATCH_SIZE=xxx 覆盖
 */
#ifndef GALAY_SCHEDULER_BATCH_SIZE
#define GALAY_SCHEDULER_BATCH_SIZE 256
#endif

namespace galay::kernel
{

/**
 * @brief 调度器分类
 * @details 用于在运行时区分 IO 驱动调度器和纯计算调度器。
 */
enum SchedulerType {
    kIOScheduler,      ///< 基于 IO 事件循环的调度器
    kComputeScheduler  ///< 基于工作线程执行的计算调度器
}; 

/**
 * @brief 协程调度器基类
 *
 * @details 定义协程调度的基本接口。
 * 所有调度器实现都必须继承此类。
 *
 * @see IOScheduler, KqueueScheduler
 */
class Scheduler {
public:
    /**
     * @brief 虚析构函数
     */
    virtual ~Scheduler() = default;

    /**
     * @brief 启动调度器
     * @return 成功返回 void；底层初始化失败时返回 IOError
     * @note 子类必须实现此方法；该接口不阻塞等待调度器退出。
     */
    virtual std::expected<void, IOError> start() = 0;

    /**
     * @brief 停止调度器
     * @note 子类必须实现此方法
     */
    virtual void stop() = 0;

    /**
     * @brief 直接提交已绑定调度器的任务引用
     * @param task 任务引用；若未绑定 owner scheduler，会绑定到当前调度器
     */
    virtual bool schedule(TaskRef task) = 0;

    /**
     * @brief 延后提交已绑定调度器的任务引用
     * @param task 任务引用；若未绑定 owner scheduler，会绑定到当前调度器
     */
    virtual bool scheduleDeferred(TaskRef task) = 0;

    /**
     * @brief 立即在当前线程恢复任务
     * @param task 任务引用；若未绑定 owner scheduler，会绑定到当前调度器
     */
    virtual bool scheduleImmediately(TaskRef task) = 0;

    /**
     * @brief 添加定时器到内部时间轮
     * @param timer 待注册的定时器对象
     * @return true 定时器已被调度器接管；false 注册失败
     */
    virtual bool addTimer(Timer::ptr timer) = 0;

    /**
     * @brief 配置或取消调度器线程绑核
     * @param cpu_id 目标 CPU 核心编号（从 0 开始）；传 std::nullopt 表示取消绑核
     * @return true 配置成功；false 参数无效或平台不支持
     * @note 默认不绑核，仅在主动调用本接口后生效
     * @note 在 start() 之前调用可保证立即生效
     */
    bool setAffinity(std::optional<uint32_t> cpu_id);

    /**
     * @brief 获取调度器所属线程ID
     * @return 线程ID
     */
    std::thread::id threadId() const { return m_threadId; }

    /**
     * @brief 返回Scheduler类型
     * @return  SchedulerType 调度器类型
     */
    virtual SchedulerType type() = 0;

protected:
    /**
     * @brief 将任务与当前调度器绑定
     * @param task 待绑定的任务引用
     * @return true 任务原本未绑定或已绑定到当前调度器；false 任务无状态或属于其他调度器
     * @note 该辅助函数只修改任务所属调度器，不负责入队或恢复执行
     */
    bool bindTask(TaskRef& task);

    /**
     * @brief 在当前线程恢复任务
     * @param task 待恢复的任务引用
     * @details 若任务记录了 Runtime，会先切换到对应 Runtime 作用域再恢复协程
     * @note 仅应由调度器执行线程调用
     */
    void resume(TaskRef& task);

    /**
     * @brief 在当前线程恢复 ready queue 中的就绪项
     * @param entry 待恢复的语言中立就绪项；C++ 任务会被转换回 TaskRef 后恢复
     * @note 当前 Task 2 仅支持 C++ TaskState，未来 C 协程将在该边界接入
     */
    void resume(detail::ReadyEntry& entry);

    /**
     * @brief 将已配置的线程绑核设置应用到当前线程
     * @return true 绑核设置已成功应用或无需应用；false 平台调用失败
     * @note 只有在 setAffinity() 设定了具体 CPU 后该函数才会实际执行绑核
     */
    bool applyConfiguredAffinity();
    std::thread::id m_threadId;  ///< 调度器所属线程ID，在 start() 时设置

private:
    static constexpr int32_t kNoAffinity = -1;
    std::atomic<int32_t> m_affinity_cpu{kNoAffinity};
};

namespace detail {

inline bool scheduleReadyEntry(ReadyEntry& entry) noexcept
{
    if (!entry.isCppTask()) {
        releaseReadyEntry(entry);
        return false;
    }

    TaskRef task = readyEntryToTaskRef(entry);
    auto* scheduler = task.belongScheduler();
    return scheduler != nullptr && scheduler->schedule(std::move(task));
}

}  // namespace detail

inline bool Scheduler::bindTask(TaskRef& task) {
    auto* state = task.state();
    if (!state) {
        return false;
    }
    if (state->m_scheduler == nullptr) {
        detail::setTaskScheduler(task, this);
        return true;
    }
    return state->m_scheduler == this;
}


inline void Scheduler::resume(TaskRef& task) {
    auto* state = task.state();
    if (!state || !state->m_handle || state->m_done.load(std::memory_order_relaxed)) {
        return;
    }
    state->m_queued.store(false, std::memory_order_relaxed);
    state->m_resume_owner_only.store(false, std::memory_order_relaxed);
    if (state->m_runtime == nullptr) {
        state->m_handle.resume();
        return;
    }
    if (state->m_runtime == detail::currentRuntime()) {
        state->m_handle.resume();
        return;
    }
    detail::CurrentRuntimeScope runtime_scope(state->m_runtime);
    state->m_handle.resume();
}

inline void Scheduler::resume(detail::ReadyEntry& entry) {
    if (!entry.isCppTask()) {
        detail::releaseReadyEntry(entry);
        return;
    }

    TaskRef task = detail::readyEntryToTaskRef(entry);
    resume(task);
}

/**
 * @brief 将 Task 所有权转交给指定调度器并立即入队
 * @tparam T Task 返回值类型
 * @param scheduler 目标调度器引用
 * @param task 待提交的协程任务
 * @return true 任务成功交给调度器；false 调度器拒绝该任务
 */
template <typename T>
inline bool scheduleTask(Scheduler& scheduler, Task<T>&& task)
{
    return scheduler.schedule(detail::TaskAccess::detachTask(std::move(task)));
}

/**
 * @brief 将 Task 所有权转交给指定调度器并立即入队
 * @tparam T Task 返回值类型
 * @param scheduler 目标调度器指针；为空时返回 false
 * @param task 待提交的协程任务
 * @return true 任务成功交给调度器；false 调度器为空或调度器拒绝该任务
 */
template <typename T>
inline bool scheduleTask(Scheduler* scheduler, Task<T>&& task)
{
    return scheduler != nullptr && scheduleTask(*scheduler, std::move(task));
}

/**
 * @brief 将 Task 所有权转交给指定调度器并延后执行
 * @tparam T Task 返回值类型
 * @param scheduler 目标调度器引用
 * @param task 待提交的协程任务
 * @return true 任务已加入延后队列；false 调度器拒绝该任务
 */
template <typename T>
inline bool scheduleTaskDeferred(Scheduler& scheduler, Task<T>&& task)
{
    return scheduler.scheduleDeferred(detail::TaskAccess::detachTask(std::move(task)));
}

/**
 * @brief 将 Task 所有权转交给指定调度器并延后执行
 * @tparam T Task 返回值类型
 * @param scheduler 目标调度器指针；为空时返回 false
 * @param task 待提交的协程任务
 * @return true 任务已加入延后队列；false 调度器为空或调度器拒绝该任务
 */
template <typename T>
inline bool scheduleTaskDeferred(Scheduler* scheduler, Task<T>&& task)
{
    return scheduler != nullptr && scheduleTaskDeferred(*scheduler, std::move(task));
}

/**
 * @brief 立即在调度器当前执行线程恢复 Task
 * @tparam T Task 返回值类型
 * @param scheduler 目标调度器引用
 * @param task 待执行的协程任务
 * @return true 任务已被当前线程恢复；false 调度器拒绝该任务
 * @note 调用方需要保证该接口符合目标调度器的线程约束
 */
template <typename T>
inline bool scheduleTaskImmediately(Scheduler& scheduler, Task<T>&& task)
{
    return scheduler.scheduleImmediately(detail::TaskAccess::detachTask(std::move(task)));
}

/**
 * @brief 立即在调度器当前执行线程恢复 Task
 * @tparam T Task 返回值类型
 * @param scheduler 目标调度器指针；为空时返回 false
 * @param task 待执行的协程任务
 * @return true 任务已被当前线程恢复；false 调度器为空或调度器拒绝该任务
 */
template <typename T>
inline bool scheduleTaskImmediately(Scheduler* scheduler, Task<T>&& task)
{
    return scheduler != nullptr && scheduleTaskImmediately(*scheduler, std::move(task));
}

} // namespace galay::kernel

#endif // GALAY_KERNEL_SCHEDULER_H
