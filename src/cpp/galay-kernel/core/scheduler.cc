/**
 * @file scheduler.cc
 * @brief 调度器基类 CPU 亲和性实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现 setAffinity()（存储目标 CPU）和 applyConfiguredAffinity()
 * （在 Linux 上通过 pthread_setaffinity_np 应用）。
 * 非 Linux 平台回退为空操作或不支持。
 */

#include "scheduler.hpp"

#include <cstddef>

#if defined(USE_KQUEUE)
#include "kqueue_scheduler.h"
#endif
#if defined(USE_EPOLL)
#include "epoll_scheduler.h"
#endif
#if defined(USE_IOURING)
#include "uring_scheduler.h"
#endif

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

bool isSchedulerThread() noexcept
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

bool scheduleReadyEntryOnScheduler(Scheduler* scheduler, ReadyEntry& entry) noexcept
{
    if (scheduler == nullptr || !entry.isValid()) {
        return false;
    }
#if defined(USE_KQUEUE)
    if (auto* kqueue = dynamic_cast<KqueueScheduler*>(scheduler)) {
        return kqueue->scheduleReadyEntry(entry);
    }
#endif
#if defined(USE_EPOLL)
    if (auto* epoll = dynamic_cast<EpollScheduler*>(scheduler)) {
        return epoll->scheduleReadyEntry(entry);
    }
#endif
#if defined(USE_IOURING)
    if (auto* uring = dynamic_cast<IOUringScheduler*>(scheduler)) {
        return uring->scheduleReadyEntry(entry);
    }
#endif
    return false;
}

} // namespace detail

/**
 * @brief 配置或清除调度器的 CPU 亲和性目标
 *
 * @param cpu_id  目标 CPU 核心索引，传 std::nullopt 清除亲和性
 * @return true 成功；false 平台不支持亲和性或 CPU 索引越界
 */
bool Scheduler::setAffinity(std::optional<uint32_t> cpu_id)
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
bool Scheduler::applyConfiguredAffinity()
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
