/**
 * @file scheduler.cc
 * @brief 内置调度器类型分派及 CPU 亲和性实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现 set_affinity()（存储目标 CPU）和 apply_configured_affinity()
 * （在 Linux 上通过 pthread_setaffinity_np 应用）。
 * 非 Linux 平台回退为空操作或不支持。
 */

#include "scheduler.hpp"
#include "scheduler_dispatch.hpp"

#include <cstddef>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace galay::kernel
{

namespace detail
{

namespace
{

thread_local bool g_is_scheduler_thread = false;

} // namespace

bool is_scheduler_thread() noexcept
{
    return g_is_scheduler_thread;
}

SchedulerThreadScope::SchedulerThreadScope() noexcept
    : m_previous(g_is_scheduler_thread)
{
    g_is_scheduler_thread = true;
}

SchedulerThreadScope::~SchedulerThreadScope()
{
    g_is_scheduler_thread = m_previous;
}

bool schedule_ready_entry(ReadyEntry& entry) noexcept
{
    if (!entry.is_valid()) {
        return false;
    }

    if (entry.is_cpp_task()) {
        auto* scheduler = ready_entry_scheduler(entry);
        if (scheduler == nullptr) {
            return false;
        }
        TaskRef task = ready_entry_to_task_ref(entry);
        TaskRef scheduled_task(task);
        if (scheduler->schedule(std::move(scheduled_task))) {
            return true;
        }
        entry = ReadyEntry(std::move(task));
        return false;
    }

    auto* scheduler = ready_entry_scheduler(entry);
    return scheduler != nullptr && schedule_ready_entry_on_scheduler(scheduler, entry);
}

bool schedule_ready_entry_on_scheduler(Scheduler* scheduler, ReadyEntry& entry) noexcept
{
    if (scheduler == nullptr || !entry.is_valid()) {
        return false;
    }
    return scheduler->schedule_ready_entry(entry);
}

} // namespace detail

/**
 * @brief 配置或清除调度器的 CPU 亲和性目标
 *
 * @param cpu_id  目标 CPU 核心索引，传 std::nullopt 清除亲和性
 * @return true 成功；false 平台不支持亲和性或 CPU 索引越界
 */
bool Scheduler::set_affinity(std::optional<uint32_t> cpu_id)
{
    if (!cpu_id.has_value()) {
        m_affinity_cpu.store(kNoAffinity, std::memory_order_release);
        return true;
    }

#if !defined(__linux__)
    (void)cpu_id;
    return false;
#else
    const uint32_t cpu_count = std::thread::hardware_concurrency();
    if (cpu_count > 0 && *cpu_id >= cpu_count) {
        return false;
    }
    m_affinity_cpu.store(static_cast<int32_t>(*cpu_id), std::memory_order_release);
    return true;
#endif
}

/**
 * @brief 将先前配置的 CPU 亲和性应用到当前线程
 *
 * @return true 亲和性已应用或无需应用；false 在 Linux 上 pthread_setaffinity_np 失败或平台不支持
 */
bool Scheduler::apply_configured_affinity()
{
    const int32_t cpu_id = m_affinity_cpu.load(std::memory_order_acquire);
    if (cpu_id < 0) {
        return true; // 默认不绑核
    }

#if defined(__linux__)
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(static_cast<size_t>(cpu_id), &cpu_set);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set) == 0;
#else
    return false;
#endif
}

} // namespace galay::kernel
