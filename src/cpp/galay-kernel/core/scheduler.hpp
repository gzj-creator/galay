/**
 * @file scheduler.hpp
 * @brief 协程调度器基类与任务调度辅助函数
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义内部 Scheduler 借用接口（start、stop、schedule、scheduleDeferred、
 * scheduleImmediately），包含处理 Runtime 作用域的共享 resume() 实现。
 * 同时提供用于便捷 Task 提交的 scheduleTask 重载函数。
 *
 * @note 具体实现：KqueueScheduler (macOS)、EpollScheduler (Linux)、
 *       IOUringScheduler (Linux)、ParallelScheduler
 */

#ifndef GALAY_KERNEL_SCHEDULER_HPP
#define GALAY_KERNEL_SCHEDULER_HPP

#include "../common/timer.hpp"
#include "../common/error.h"
#include "../common/kernel_config.h"
#include "../common/scheduler_config.h"
#include "task.h"
#include <atomic>
#include <cstdint>
#include <expected>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#ifdef USE_IOURING
#include <linux/time_types.h>
#endif

namespace galay::kernel
{

/**
 * @brief 调度器分类
 * @details 用于在运行时区分 IO 驱动调度器和纯计算调度器。
 */
enum SchedulerType {
    kIOScheduler,      ///< 基于 IO 事件循环的调度器
    kParallelScheduler  ///< 基于工作线程执行的计算调度器
};

// 前向声明模板类
template <typename Config> class EpollSchedulerT;
template <typename Config> class KqueueSchedulerT;
template <typename Config> class IOUringSchedulerT;
class ParallelScheduler;
class Scheduler;

// 默认调度器类型别名
using EpollScheduler = EpollSchedulerT<DefaultIOSchedulerConfig>;
using KqueueScheduler = KqueueSchedulerT<DefaultIOSchedulerConfig>;
using IOUringScheduler = IOUringSchedulerT<DefaultIOSchedulerConfig>;

#if defined(USE_IOURING)
using IOScheduler = IOUringScheduler;
#elif defined(USE_KQUEUE)
using IOScheduler = KqueueScheduler;
#elif defined(USE_EPOLL)
using IOScheduler = EpollScheduler;
#endif

template <typename Derived, SchedulerType Type> class SchedulerBase;
template <typename Derived, typename Reactor> class IOSchedulerBase;
#ifdef GALAY_KERNEL_TEST_SCHEDULER
namespace detail {
struct SchedulerTestHooks;
template <typename Derived> class SchedulerTestAdapter;
}
#endif

/**
 * @brief 协程调度器基类
 *
 * @details Task/Waker 保存的非拥有借用。仅允许内置调度器构造，不能通过此
 * 类型删除对象。统一入口按调度器类型转到 SchedulerBase 的静态 Derived* 函数。
 *
 * @see IOScheduler, KqueueScheduler
 */
class Scheduler {
public:
    /**
     * @brief 启动调度器
     * @return 成功返回 void；底层初始化失败时返回 IOError
     * @note 仅分派到内置后端；该接口不阻塞等待调度器退出。
     */
    std::expected<void, IOError> start();

    /**
     * @brief 停止调度器
     * @note 调用方必须先关闭异步源并等候所有借用该调度器的任务退出。
     */
    void stop();

    /**
     * @brief 直接提交已绑定调度器的任务引用
     * @param task 任务引用；若未绑定 owner scheduler，会绑定到当前调度器
     */
    bool schedule(TaskRef task) noexcept;

    /**
     * @brief 接纳已停泊任务的恢复请求。
     * @param task 已绑定到本调度器的停泊任务引用。
     * @return live scheduler 成功接管任务返回 true；无效、owner 不匹配或调度器
     *         已停止接纳恢复时返回 false。
     * @details 实现必须无分配、不可抛异常，并保证成功接纳的任务只在 owner
     *          scheduler 线程恢复。该入口供 Waker/timeout/completion 使用，不能
     *          回退为跨线程内联恢复。
     * @note 调用方必须在停止或销毁 scheduler 前关闭异步源并等待所有 waiter 退出。
     */
    bool scheduleResume(TaskRef task) noexcept;

    /**
     * @brief 延后提交已绑定调度器的任务引用
     * @param task 任务引用；若未绑定 owner scheduler，会绑定到当前调度器
     */
    bool scheduleDeferred(TaskRef task) noexcept;

    /**
     * @brief 立即在当前线程恢复任务
     * @param task 任务引用；若未绑定 owner scheduler，会绑定到当前调度器
     */
    bool scheduleImmediately(TaskRef task) noexcept;

    /**
     * @brief 将语言中立 ready entry 投递到该调度器。
     * @details C stackful coroutine 使用该入口，避免热路径通过 RTTI 识别具体
     *          IOScheduler 后端；非 IO 调度器默认拒绝该入口。
     */
    bool scheduleReadyEntry(detail::ReadyEntry& entry) noexcept;

    /**
     * @brief 添加定时器到内部时间轮
     * @param timer 待注册的定时器对象
     * @return true 定时器已被调度器接管；false 注册失败
     */
    bool addTimer(Timer::ptr timer);

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
    SchedulerType type() const noexcept { return m_type; }

protected:
    ~Scheduler() = default;
    /**
     * @brief 将任务与当前调度器绑定
     * @param task 待绑定的任务引用
     * @return true 任务原本未绑定或已绑定到当前调度器；false 任务无状态或属于其他调度器
     * @note 该辅助函数只修改任务所属调度器，不负责入队或恢复执行
     */
    bool bindTask(TaskRef& task) noexcept;

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
     * @note C 协程通过 ReadyEntry hook 恢复；缺失 hook 时安全拒绝并保留 entry。
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
    template <typename Derived, SchedulerType Type> friend class SchedulerBase;
    explicit Scheduler(SchedulerType type) noexcept : m_type(type) {}
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    template <typename Derived> friend class detail::SchedulerTestAdapter;
    const detail::SchedulerTestHooks* m_test_hooks = nullptr;
#endif
    static constexpr int32_t kNoAffinity = -1;
    std::atomic<int32_t> m_affinity_cpu{kNoAffinity};
    const SchedulerType m_type;
};

/**
 * @brief 内置调度器的 CRTP 入口及供 Scheduler 借用指针使用的静态分派函数。
 * @note Impl 方法只由相应具体调度器实现。调度器地址必须在任务存活期间保持稳定。
 */
template <typename Derived, SchedulerType Type>
class SchedulerBase : public Scheduler {
    // 检查是否是模板调度器类型
    template <typename T, template <typename> class Template>
    struct is_specialization_of : std::false_type {};

    template <template <typename> class Template, typename... Args>
    struct is_specialization_of<Template<Args...>, Template> : std::true_type {};

    static_assert((Type == kIOScheduler &&
                   (is_specialization_of<Derived, EpollSchedulerT>::value ||
                    is_specialization_of<Derived, KqueueSchedulerT>::value ||
                    is_specialization_of<Derived, IOUringSchedulerT>::value)) ||
                  (Type == kParallelScheduler && std::is_same_v<Derived, ParallelScheduler>));
public:
    std::expected<void, IOError> start() { return start(&derived()); }
    void stop() { stop(&derived()); }
    bool schedule(TaskRef task) noexcept { return schedule(&derived(), std::move(task)); }
    bool scheduleResume(TaskRef task) noexcept { return scheduleResume(&derived(), std::move(task)); }
    bool scheduleDeferred(TaskRef task) noexcept { return scheduleDeferred(&derived(), std::move(task)); }
    bool scheduleImmediately(TaskRef task) noexcept { return scheduleImmediately(&derived(), std::move(task)); }
    bool scheduleReadyEntry(detail::ReadyEntry& entry) noexcept {
        return scheduleReadyEntry(&derived(), entry);
    }
    bool addTimer(Timer::ptr timer) { return addTimer(&derived(), std::move(timer)); }

    static std::expected<void, IOError> start(Derived* scheduler) {
        return scheduler->startImpl();
    }
    static void stop(Derived* scheduler) { scheduler->stopImpl(); }
    static bool schedule(Derived* scheduler, TaskRef task) noexcept {
        return scheduler->scheduleImpl(std::move(task));
    }
    static bool scheduleResume(Derived* scheduler, TaskRef task) noexcept {
        return scheduler->scheduleResumeImpl(std::move(task));
    }
    static bool scheduleDeferred(Derived* scheduler, TaskRef task) noexcept {
        return scheduler->scheduleDeferredImpl(std::move(task));
    }
    static bool scheduleImmediately(Derived* scheduler, TaskRef task) noexcept {
        return scheduler->scheduleImmediatelyImpl(std::move(task));
    }
    static bool scheduleReadyEntry(Derived* scheduler,
                                   detail::ReadyEntry& entry) noexcept {
        if constexpr (Type == kIOScheduler) {
            return scheduler->scheduleReadyEntryImpl(entry);
        } else {
            return false; // compute 调度器不接纳 C stackful ready entry。
        }
    }
    static bool addTimer(Derived* scheduler, Timer::ptr timer) {
        return scheduler->addTimerImpl(std::move(timer));
    }
    static constexpr SchedulerType type() noexcept { return Type; }

protected:
    ~SchedulerBase() = default;

private:
    friend Derived;
    template <typename, typename> friend class IOSchedulerBase;
    SchedulerBase() noexcept : Scheduler(Type) {}
    Derived& derived() noexcept { return static_cast<Derived&>(*this); }
};

#ifdef GALAY_KERNEL_TEST_SCHEDULER
namespace detail {
// 仅确定性竞态测试的独立库启用；生产对象布局和热路径不包含这些 hook。
struct SchedulerTestHooks {
    std::expected<void, IOError> (*start)(Scheduler*);
    void (*stop)(Scheduler*);
    bool (*schedule)(Scheduler*, TaskRef) noexcept;
    bool (*scheduleResume)(Scheduler*, TaskRef) noexcept;
    bool (*scheduleDeferred)(Scheduler*, TaskRef) noexcept;
    bool (*scheduleImmediately)(Scheduler*, TaskRef) noexcept;
    bool (*addTimer)(Scheduler*, Timer::ptr);
};
}
#endif

namespace detail {

bool isSchedulerThread() noexcept;

class SchedulerThreadScope
{
public:
    SchedulerThreadScope() noexcept;
    ~SchedulerThreadScope();

    SchedulerThreadScope(const SchedulerThreadScope&) = delete;
    SchedulerThreadScope& operator=(const SchedulerThreadScope&) = delete;

private:
    bool m_previous;
};

bool scheduleReadyEntryOnScheduler(Scheduler* scheduler, ReadyEntry& entry) noexcept;

bool scheduleReadyEntry(ReadyEntry& entry) noexcept;

}  // namespace detail

inline bool Scheduler::bindTask(TaskRef& task) noexcept {
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
    (void)detail::resumeTaskState(state);
}

inline void Scheduler::resume(detail::ReadyEntry& entry) {
    (void)detail::resumeReadyEntry(entry);
}

/**
 * @brief 将 Task 所有权转交给指定调度器并立即入队
 * @tparam T Task 返回值类型
 * @param scheduler 目标调度器引用
 * @param task 待提交的协程任务
 * @return true 任务成功交给调度器；false 调度器拒绝该任务
 */
template <typename T, typename SchedulerT>
    requires std::is_base_of_v<Scheduler, SchedulerT>
inline bool scheduleTask(SchedulerT& scheduler, Task<T>&& task)
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
template <typename T, typename SchedulerT>
    requires std::is_base_of_v<Scheduler, SchedulerT>
inline bool scheduleTask(SchedulerT* scheduler, Task<T>&& task)
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
template <typename T, typename SchedulerT>
    requires std::is_base_of_v<Scheduler, SchedulerT>
inline bool scheduleTaskDeferred(SchedulerT& scheduler, Task<T>&& task)
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
template <typename T, typename SchedulerT>
    requires std::is_base_of_v<Scheduler, SchedulerT>
inline bool scheduleTaskDeferred(SchedulerT* scheduler, Task<T>&& task)
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
template <typename T, typename SchedulerT>
    requires std::is_base_of_v<Scheduler, SchedulerT>
inline bool scheduleTaskImmediately(SchedulerT& scheduler, Task<T>&& task)
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
template <typename T, typename SchedulerT>
    requires std::is_base_of_v<Scheduler, SchedulerT>
inline bool scheduleTaskImmediately(SchedulerT* scheduler, Task<T>&& task)
{
    return scheduler != nullptr && scheduleTaskImmediately(*scheduler, std::move(task));
}

} // namespace galay::kernel

#endif // GALAY_KERNEL_SCHEDULER_H
