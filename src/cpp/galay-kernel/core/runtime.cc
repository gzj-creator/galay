/**
 * @file runtime.cc
 * @brief 运行时调度器管理实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现 Runtime 生命周期管理、默认调度器创建、亲和性配置、
 * 工作窃取域设置以及线程局部 RuntimeHandle 访问器。
 */

#include "runtime.h"
#include "timer_scheduler.h"
#include <span>
#include <thread>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include "kqueue_scheduler.h"
using DefaultIOScheduler = galay::kernel::KqueueScheduler;
#elif defined(__linux__)
#ifdef USE_IOURING
#include "uring_scheduler.h"
using DefaultIOScheduler = galay::kernel::IOUringScheduler;
#else
#include "epoll_scheduler.h"
using DefaultIOScheduler = galay::kernel::EpollScheduler;
#endif
#endif

namespace galay::kernel
{

Runtime::Runtime(const RuntimeConfig& config)
    : m_config(config)
{
}

Runtime::~Runtime()
{
    stop();
}

std::expected<void, RuntimeError> Runtime::start()
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return {};
    }

    if (m_io_schedulers.empty() && m_parallel_schedulers.empty()) {
        create_default_schedulers();
    }

    apply_affinity_config();
    configure_io_scheduler_steal_domains();

    TimerScheduler::get_instance()->start();
    for (auto& scheduler : m_io_schedulers) {
        auto started = scheduler->start();
        if (!started.has_value()) {
            stop();
            return std::unexpected(RuntimeError(RuntimeErrorCode::kSchedulerStartFailed));
        }
    }
    for (auto& scheduler : m_parallel_schedulers) {
        auto started = scheduler->start();
        if (!started.has_value()) {
            stop();
            return std::unexpected(RuntimeError(RuntimeErrorCode::kSchedulerStartFailed));
        }
    }
    return {};
}

void Runtime::stop()
{
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;
    }

    // 在 scheduler 仍存活时排空阻塞任务，避免停机期间丢失完成唤醒。
    m_blockingExecutor.stop();

    for (auto it = m_parallel_schedulers.rbegin(); it != m_parallel_schedulers.rend(); ++it) {
        (*it)->stop();
    }
    for (auto it = m_io_schedulers.rbegin(); it != m_io_schedulers.rend(); ++it) {
        (*it)->stop();
    }
    TimerScheduler::get_instance()->stop();
}

RuntimeHandle Runtime::handle() noexcept
{
    return RuntimeHandle(this);
}

RuntimeStats Runtime::stats() const
{
    RuntimeStats snapshot;
    snapshot.io_schedulers.reserve(m_io_schedulers.size());
    for (const auto& scheduler : m_io_schedulers) {
        snapshot.io_schedulers.push_back(
            scheduler ? scheduler->steal_stats() : IOSchedulerStealStats{});
    }
    return snapshot;
}

IOScheduler* Runtime::get_io_scheduler(size_t index)
{
    return index < m_io_schedulers.size() ? m_io_schedulers[index].get() : nullptr;
}

ParallelScheduler* Runtime::get_parallel_scheduler(size_t index)
{
    return index < m_parallel_schedulers.size() ? m_parallel_schedulers[index].get() : nullptr;
}

IOScheduler* Runtime::get_next_io_scheduler()
{
    if (m_io_schedulers.empty()) {
        return nullptr;
    }
    return m_io_schedulers[m_io_index.fetch_add(1, std::memory_order_relaxed) % m_io_schedulers.size()].get();
}

ParallelScheduler* Runtime::get_next_parallel_scheduler()
{
    if (m_parallel_schedulers.empty()) {
        return nullptr;
    }
    return m_parallel_schedulers[m_parallel_index.fetch_add(1, std::memory_order_relaxed) % m_parallel_schedulers.size()].get();
}

std::expected<void, RuntimeError> Runtime::ensure_started()
{
    if (!is_running()) {
        return start();
    }
    return {};
}

std::expected<IOScheduler*, RuntimeError> Runtime::acquire_io_scheduler()
{
    auto started = ensure_started();
    if (!started.has_value()) {
        return std::unexpected(started.error());
    }
    return get_next_io_scheduler();
}

std::expected<ParallelScheduler*, RuntimeError> Runtime::acquire_parallel_scheduler()
{
    auto started = ensure_started();
    if (!started.has_value()) {
        return std::unexpected(started.error());
    }
    return get_next_parallel_scheduler();
}

void Runtime::bind_task_to_runtime(const TaskRef& task, Scheduler* scheduler)
{
    detail::set_task_runtime(task, this);
    detail::set_task_scheduler(task, scheduler);
}

RuntimeError Runtime::map_task_result_error(const detail::TaskResultError& error) noexcept
{
    if (error.code() == detail::TaskResultErrorCode::kTaskException) {
        return RuntimeError(RuntimeErrorCode::kTaskException);
    }
    if (error.code() == detail::TaskResultErrorCode::kResumeFailed) {
        return RuntimeError(RuntimeErrorCode::kResumeFailed);
    }
    return RuntimeError(RuntimeErrorCode::kSubmitFailed);
}

std::expected<RuntimeHandle, RuntimeError> RuntimeHandle::current()
{
    auto* runtime = detail::current_runtime();
    if (runtime == nullptr) {
        return std::unexpected(RuntimeError(RuntimeErrorCode::kNoCurrentRuntime));
    }
    return RuntimeHandle(runtime);
}

std::optional<RuntimeHandle> RuntimeHandle::try_current()
{
    if (auto* runtime = detail::current_runtime()) {
        return RuntimeHandle(runtime);
    }
    return std::nullopt;
}

size_t Runtime::get_cpu_count()
{
    size_t count = std::thread::hardware_concurrency();
    return count > 0 ? count : 4;
}

void Runtime::create_default_schedulers()
{
    size_t cpu = get_cpu_count();
    const size_t ioCount = m_config.io_scheduler_count == GALAY_RUNTIME_SCHEDULER_COUNT_AUTO
        ? cpu * 2
        : m_config.io_scheduler_count;
    const size_t parallelCount = m_config.parallel_scheduler_count == GALAY_RUNTIME_SCHEDULER_COUNT_AUTO
        ? cpu
        : m_config.parallel_scheduler_count;

    for (size_t i = 0; i < ioCount; ++i) {
        m_io_schedulers.push_back(std::make_unique<DefaultIOScheduler>());
    }
    for (size_t i = 0; i < parallelCount; ++i) {
        m_parallel_schedulers.push_back(std::make_unique<ParallelScheduler>());
    }
}

void Runtime::apply_affinity_config()
{
    const auto& affinity = m_config.affinity;
    if (affinity.mode == RuntimeAffinityConfig::Mode::None) {
        return;
    }

    const uint32_t cpuCount = static_cast<uint32_t>(get_cpu_count());

    if (affinity.mode == RuntimeAffinityConfig::Mode::Sequential) {
        uint32_t cpu = 0;
        for (size_t i = 0; i < affinity.seq_io_count && i < m_io_schedulers.size(); ++i) {
            m_io_schedulers[i]->set_affinity(cpu % cpuCount);
            ++cpu;
        }
        cpu = 0;
        for (size_t i = 0; i < affinity.seq_parallel_count && i < m_parallel_schedulers.size(); ++i) {
            m_parallel_schedulers[i]->set_affinity(cpu % cpuCount);
            ++cpu;
        }
        return;
    }

    if (affinity.custom_io_cpus.size() != m_io_schedulers.size() ||
        affinity.custom_parallel_cpus.size() != m_parallel_schedulers.size()) {
        return;
    }

    for (size_t i = 0; i < m_io_schedulers.size(); ++i) {
        m_io_schedulers[i]->set_affinity(affinity.custom_io_cpus[i]);
    }
    for (size_t i = 0; i < m_parallel_schedulers.size(); ++i) {
        m_parallel_schedulers[i]->set_affinity(affinity.custom_parallel_cpus[i]);
    }
}

void Runtime::configure_io_scheduler_steal_domains()
{
    const size_t io_count = m_io_schedulers.size();
    if (io_count == 0) {
        m_io_scheduler_sibling_view.clear();
        return;
    }

    m_io_scheduler_sibling_view.clear();
    m_io_scheduler_sibling_view.reserve(io_count);
    for (auto& scheduler : m_io_schedulers) {
        m_io_scheduler_sibling_view.push_back(scheduler.get());
    }

    const std::span<IOScheduler* const> siblings{m_io_scheduler_sibling_view.data(), m_io_scheduler_sibling_view.size()};
    for (size_t index = 0; index < siblings.size(); ++index) {
        siblings[index]->configure_steal_domain(siblings, index);
    }
}

} // namespace galay::kernel
