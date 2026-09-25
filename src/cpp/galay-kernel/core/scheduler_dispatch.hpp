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
        return SchedulerBase<IOScheduler, kIOScheduler>::start(
            static_cast<IOScheduler*>(this));
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
        SchedulerBase<IOScheduler, kIOScheduler>::stop(static_cast<IOScheduler*>(this));
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
        return SchedulerBase<IOScheduler, kIOScheduler>::schedule(
            static_cast<IOScheduler*>(this), std::move(task));
    }
    return SchedulerBase<ParallelScheduler, kParallelScheduler>::schedule(
        static_cast<ParallelScheduler*>(this), std::move(task));
}

inline bool Scheduler::scheduleResume(TaskRef task) noexcept
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return m_test_hooks->scheduleResume(this, std::move(task)); }
#endif
    if (m_type == kIOScheduler) {
        return SchedulerBase<IOScheduler, kIOScheduler>::scheduleResume(
            static_cast<IOScheduler*>(this), std::move(task));
    }
    return SchedulerBase<ParallelScheduler, kParallelScheduler>::scheduleResume(
        static_cast<ParallelScheduler*>(this), std::move(task));
}

inline bool Scheduler::scheduleDeferred(TaskRef task) noexcept
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return m_test_hooks->scheduleDeferred(this, std::move(task)); }
#endif
    if (m_type == kIOScheduler) {
        return SchedulerBase<IOScheduler, kIOScheduler>::scheduleDeferred(
            static_cast<IOScheduler*>(this), std::move(task));
    }
    return SchedulerBase<ParallelScheduler, kParallelScheduler>::scheduleDeferred(
        static_cast<ParallelScheduler*>(this), std::move(task));
}

inline bool Scheduler::scheduleImmediately(TaskRef task) noexcept
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return m_test_hooks->scheduleImmediately(this, std::move(task)); }
#endif
    if (m_type == kIOScheduler) {
        return SchedulerBase<IOScheduler, kIOScheduler>::scheduleImmediately(
            static_cast<IOScheduler*>(this), std::move(task));
    }
    return SchedulerBase<ParallelScheduler, kParallelScheduler>::scheduleImmediately(
        static_cast<ParallelScheduler*>(this), std::move(task));
}

inline bool Scheduler::scheduleReadyEntry(detail::ReadyEntry& entry) noexcept
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return false; }
#endif
    if (m_type != kIOScheduler) { return false; }
    return SchedulerBase<IOScheduler, kIOScheduler>::scheduleReadyEntry(
        static_cast<IOScheduler*>(this), entry);
}

inline bool Scheduler::addTimer(Timer::ptr timer)
{
#ifdef GALAY_KERNEL_TEST_SCHEDULER
    if (m_test_hooks) { return m_test_hooks->addTimer(this, std::move(timer)); }
#endif
    if (m_type == kIOScheduler) {
        return SchedulerBase<IOScheduler, kIOScheduler>::addTimer(
            static_cast<IOScheduler*>(this), std::move(timer));
    }
    return SchedulerBase<ParallelScheduler, kParallelScheduler>::addTimer(
        static_cast<ParallelScheduler*>(this), std::move(timer));
}

} // namespace galay::kernel

#endif
