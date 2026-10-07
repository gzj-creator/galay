/**
 * @file scheduler_dispatch.hpp
 * @brief 把 Scheduler 的类型分派放在头文件里，供调用方内联。
 * @details 具体 Impl 仍由各调度器实现。此处只保留按 m_type 转到 CRTP 静态入口的薄层。
 */
#ifndef GALAY_KERNEL_SCHEDULER_DISPATCH_HPP
#define GALAY_KERNEL_SCHEDULER_DISPATCH_HPP

#include "io_scheduler.hpp"
#include "../parallel/parallel_scheduler.h"

namespace galay::kernel {

inline std::expected<void, IOError> Scheduler::start()
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return m_test_hooks->start(this); }
#endif
    if (m_type == kIOScheduler) {
        return SchedulerBase<IOSchedulerBackend, kIOScheduler>::start(
            static_cast<IOSchedulerBackend*>(this));
    }
    return SchedulerBase<ParallelScheduler, kParallelScheduler>::start(
        static_cast<ParallelScheduler*>(this));
}

inline void Scheduler::stop()
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { m_test_hooks->stop(this); return; }
#endif
    if (m_type == kIOScheduler) {
        SchedulerBase<IOSchedulerBackend, kIOScheduler>::stop(static_cast<IOSchedulerBackend*>(this));
    } else {
        SchedulerBase<ParallelScheduler, kParallelScheduler>::stop(
            static_cast<ParallelScheduler*>(this));
    }
}

inline bool Scheduler::schedule(TaskRef task) noexcept
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return m_test_hooks->schedule(this, std::move(task)); }
#endif
    if (m_type == kIOScheduler) {
        return SchedulerBase<IOSchedulerBackend, kIOScheduler>::schedule(
            static_cast<IOSchedulerBackend*>(this), std::move(task));
    }
    return SchedulerBase<ParallelScheduler, kParallelScheduler>::schedule(
        static_cast<ParallelScheduler*>(this), std::move(task));
}

inline bool Scheduler::schedule_resume(TaskRef task) noexcept
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return m_test_hooks->schedule_resume(this, std::move(task)); }
#endif
    if (m_type == kIOScheduler) {
        return SchedulerBase<IOSchedulerBackend, kIOScheduler>::schedule_resume(
            static_cast<IOSchedulerBackend*>(this), std::move(task));
    }
    return SchedulerBase<ParallelScheduler, kParallelScheduler>::schedule_resume(
        static_cast<ParallelScheduler*>(this), std::move(task));
}

inline bool Scheduler::schedule_deferred(TaskRef task) noexcept
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return m_test_hooks->schedule_deferred(this, std::move(task)); }
#endif
    if (m_type == kIOScheduler) {
        return SchedulerBase<IOSchedulerBackend, kIOScheduler>::schedule_deferred(
            static_cast<IOSchedulerBackend*>(this), std::move(task));
    }
    return SchedulerBase<ParallelScheduler, kParallelScheduler>::schedule_deferred(
        static_cast<ParallelScheduler*>(this), std::move(task));
}

inline bool Scheduler::schedule_immediately(TaskRef task) noexcept
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return m_test_hooks->schedule_immediately(this, std::move(task)); }
#endif
    if (m_type == kIOScheduler) {
        return SchedulerBase<IOSchedulerBackend, kIOScheduler>::schedule_immediately(
            static_cast<IOSchedulerBackend*>(this), std::move(task));
    }
    return SchedulerBase<ParallelScheduler, kParallelScheduler>::schedule_immediately(
        static_cast<ParallelScheduler*>(this), std::move(task));
}

inline bool Scheduler::schedule_ready_entry(detail::ReadyEntry& entry) noexcept
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return false; }
#endif
    if (m_type != kIOScheduler) { return false; }
    return SchedulerBase<IOSchedulerBackend, kIOScheduler>::schedule_ready_entry(
        static_cast<IOSchedulerBackend*>(this), entry);
}

inline bool Scheduler::add_timer(Timer::ptr timer)
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return m_test_hooks->add_timer(this, std::move(timer)); }
#endif
    if (m_type == kIOScheduler) {
        return SchedulerBase<IOSchedulerBackend, kIOScheduler>::add_timer(
            static_cast<IOSchedulerBackend*>(this), std::move(timer));
    }
    return SchedulerBase<ParallelScheduler, kParallelScheduler>::add_timer(
        static_cast<ParallelScheduler*>(this), std::move(timer));
}

} // namespace galay::kernel

#endif
