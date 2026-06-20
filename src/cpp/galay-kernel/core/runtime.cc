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

bool Runtime::addIOScheduler(std::unique_ptr<IOScheduler> scheduler)
{
    if (m_running.load(std::memory_order_acquire)) {
        return false;
    }
    m_io_schedulers.push_back(std::move(scheduler));
    return true;
}

bool Runtime::addComputeScheduler(std::unique_ptr<ComputeScheduler> scheduler)
{
    if (m_running.load(std::memory_order_acquire)) {
        return false;
    }
    m_compute_schedulers.push_back(std::move(scheduler));
    return true;
}

void Runtime::start()
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    if (m_io_schedulers.empty() && m_compute_schedulers.empty()) {
        createDefaultSchedulers();
    }

    applyAffinityConfig();
    configureIOSchedulerStealDomains();

    TimerScheduler::getInstance()->start();
    for (auto& scheduler : m_io_schedulers) {
        scheduler->start();
    }
    for (auto& scheduler : m_compute_schedulers) {
        scheduler->start();
    }
}

void Runtime::stop()
{
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;
    }

    for (auto it = m_compute_schedulers.rbegin(); it != m_compute_schedulers.rend(); ++it) {
        (*it)->stop();
    }
    for (auto it = m_io_schedulers.rbegin(); it != m_io_schedulers.rend(); ++it) {
        (*it)->stop();
    }
    TimerScheduler::getInstance()->stop();
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
            scheduler ? scheduler->stealStats() : IOSchedulerStealStats{});
    }
    return snapshot;
}

IOScheduler* Runtime::getIOScheduler(size_t index)
{
    return index < m_io_schedulers.size() ? m_io_schedulers[index].get() : nullptr;
}

ComputeScheduler* Runtime::getComputeScheduler(size_t index)
{
    return index < m_compute_schedulers.size() ? m_compute_schedulers[index].get() : nullptr;
}

IOScheduler* Runtime::getNextIOScheduler()
{
    if (m_io_schedulers.empty()) {
        return nullptr;
    }
    return m_io_schedulers[m_io_index.fetch_add(1, std::memory_order_relaxed) % m_io_schedulers.size()].get();
}

ComputeScheduler* Runtime::getNextComputeScheduler()
{
    if (m_compute_schedulers.empty()) {
        return nullptr;
    }
    return m_compute_schedulers[m_compute_index.fetch_add(1, std::memory_order_relaxed) % m_compute_schedulers.size()].get();
}

void Runtime::ensureStarted()
{
    if (!isRunning()) {
        start();
    }
}

Scheduler* Runtime::acquireDefaultScheduler()
{
    ensureStarted();
    if (auto* scheduler = getNextComputeScheduler()) {
        return scheduler;
    }
    return getNextIOScheduler();
}

void Runtime::bindTaskToRuntime(const TaskRef& task, Scheduler* scheduler)
{
    detail::setTaskRuntime(task, this);
    detail::setTaskScheduler(task, scheduler);
}

bool Runtime::submitTask(const TaskRef& task)
{
    return detail::scheduleTask(task);
}

RuntimeError Runtime::mapTaskResultError(const detail::TaskResultError& error) noexcept
{
    if (error.code() == detail::TaskResultErrorCode::kTaskException) {
        return RuntimeError(RuntimeErrorCode::kTaskException);
    }
    return RuntimeError(RuntimeErrorCode::kSubmitFailed);
}

std::expected<RuntimeHandle, RuntimeError> RuntimeHandle::current()
{
    auto* runtime = detail::currentRuntime();
    if (runtime == nullptr) {
        return std::unexpected(RuntimeError(RuntimeErrorCode::kNoCurrentRuntime));
    }
    return RuntimeHandle(runtime);
}

std::optional<RuntimeHandle> RuntimeHandle::tryCurrent()
{
    if (auto* runtime = detail::currentRuntime()) {
        return RuntimeHandle(runtime);
    }
    return std::nullopt;
}

size_t Runtime::getCPUCount()
{
    size_t count = std::thread::hardware_concurrency();
    return count > 0 ? count : 4;
}

void Runtime::createDefaultSchedulers()
{
    size_t cpu = getCPUCount();
    const size_t ioCount = m_config.io_scheduler_count == GALAY_RUNTIME_SCHEDULER_COUNT_AUTO
        ? cpu * 2
        : m_config.io_scheduler_count;
    const size_t computeCount = m_config.compute_scheduler_count == GALAY_RUNTIME_SCHEDULER_COUNT_AUTO
        ? cpu
        : m_config.compute_scheduler_count;

    for (size_t i = 0; i < ioCount; ++i) {
        m_io_schedulers.push_back(std::make_unique<DefaultIOScheduler>());
    }
    for (size_t i = 0; i < computeCount; ++i) {
        m_compute_schedulers.push_back(std::make_unique<ComputeScheduler>());
    }
}

void Runtime::applyAffinityConfig()
{
    const auto& affinity = m_config.affinity;
    if (affinity.mode == RuntimeAffinityConfig::Mode::None) {
        return;
    }

    const uint32_t cpuCount = static_cast<uint32_t>(getCPUCount());

    if (affinity.mode == RuntimeAffinityConfig::Mode::Sequential) {
        uint32_t cpu = 0;
        for (size_t i = 0; i < affinity.seq_io_count && i < m_io_schedulers.size(); ++i) {
            m_io_schedulers[i]->setAffinity(cpu % cpuCount);
            ++cpu;
        }
        cpu = 0;
        for (size_t i = 0; i < affinity.seq_compute_count && i < m_compute_schedulers.size(); ++i) {
            m_compute_schedulers[i]->setAffinity(cpu % cpuCount);
            ++cpu;
        }
        return;
    }

    if (affinity.custom_io_cpus.size() != m_io_schedulers.size() ||
        affinity.custom_compute_cpus.size() != m_compute_schedulers.size()) {
        return;
    }

    for (size_t i = 0; i < m_io_schedulers.size(); ++i) {
        m_io_schedulers[i]->setAffinity(affinity.custom_io_cpus[i]);
    }
    for (size_t i = 0; i < m_compute_schedulers.size(); ++i) {
        m_compute_schedulers[i]->setAffinity(affinity.custom_compute_cpus[i]);
    }
}

void Runtime::configureIOSchedulerStealDomains()
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
        siblings[index]->configureStealDomain(siblings, index);
    }
}

} // namespace galay::kernel
