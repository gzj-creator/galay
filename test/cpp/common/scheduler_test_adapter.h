#ifndef GALAY_TEST_SCHEDULER_ADAPTER_H
#define GALAY_TEST_SCHEDULER_ADAPTER_H

#ifndef GALAY_KERNEL_TEST_SCHEDULER
#error "Deterministic scheduler tests must link galay-kernel-test-scheduler"
#endif

#include <galay/cpp/galay-kernel/core/scheduler.hpp>

namespace galay::kernel::detail {

/**
 * @brief 仅供竞态测试控制恢复和 timer 注册窗口，不属于生产调度器扩展接口。
 * @note 测试对象与独立测试库使用相同布局；不会把 hook 编译进正常 kernel。
 */
template <typename Derived>
class SchedulerTestAdapter : public Scheduler {
protected:
    SchedulerTestAdapter() noexcept : Scheduler(kParallelScheduler) { m_test_hooks = &kHooks; }
    ~SchedulerTestAdapter() = default;

private:
    static inline const SchedulerTestHooks kHooks{
        .start = [](Scheduler* self) { return static_cast<Derived*>(self)->start(); },
        .stop = [](Scheduler* self) { static_cast<Derived*>(self)->stop(); },
        .schedule = [](Scheduler* self, TaskRef task) noexcept {
            return static_cast<Derived*>(self)->schedule(std::move(task));
        },
        .schedule_resume = [](Scheduler* self, TaskRef task) noexcept {
            return static_cast<Derived*>(self)->schedule_resume(std::move(task));
        },
        .schedule_deferred = [](Scheduler* self, TaskRef task) noexcept {
            return static_cast<Derived*>(self)->schedule_deferred(std::move(task));
        },
        .schedule_immediately = [](Scheduler* self, TaskRef task) noexcept {
            return static_cast<Derived*>(self)->schedule_immediately(std::move(task));
        },
        .add_timer = [](Scheduler* self, Timer::ptr timer) {
            return static_cast<Derived*>(self)->add_timer(std::move(timer));
        },
    };
};

} // namespace galay::kernel::detail
#endif
