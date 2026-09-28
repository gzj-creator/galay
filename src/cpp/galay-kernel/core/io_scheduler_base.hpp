/**
 * @file io_scheduler_base.hpp
 * @brief 三个 IO 后端共用的调度器实现。
 * @details 统一持有线程、就绪队列、时间轮及 reactor；Derived 仅实现
 * pollBackend() 和 flushBackend()，把平台的超时格式及批量提交差异留在后端。
 * @note schedule* 可跨线程提交；IO 注册和时间轮只由 owner 线程使用。
 * start/stop 是同步生命周期边界，不可在其自身工作线程内调用 stop。
 */
#ifndef GALAY_KERNEL_IO_SCHEDULER_BASE_HPP
#define GALAY_KERNEL_IO_SCHEDULER_BASE_HPP

#include "scheduler.hpp"
#include "scheduler_core.h"
#include "sched_loop.hpp"
#include "backend_reactor.h"
#include "io_controller.hpp"
#include "../common/timer_manager.hpp"
#include <future>

namespace galay::kernel {

struct SchedulerTestAccess;

template <typename Derived, typename Reactor>
class IOSchedulerBase : public SchedulerBase<Derived, kIOScheduler>
{
    friend Derived;
    friend class SchedulerBase<Derived, kIOScheduler>;
    friend struct SchedulerTestAccess;

    explicit IOSchedulerBase(int reactor_capacity, int batch_size)
        : m_worker(static_cast<size_t>(batch_size))
        , m_wake_coordinator(m_sleeping, m_wakeup_pending)
        , m_core(m_worker, static_cast<size_t>(batch_size))
        , m_reactor(reactor_capacity, m_last_error_code)
        , m_batch_size(batch_size)
    {
        // IO 注册和 reactor 提交保持 owner 线程亲和，禁止跨线程窃取执行。
        m_worker.setStealingEnabled(false);
        m_worker.closeResumeAdmission();
    }

public:
    /**
     * @brief 替换调度器的定时器管理器
     * @param manager 新的时间轮定时器管理器（右值引用）
     * @details 用于自定义时间轮配置（wheelSize、tickDuration）以适应不同的超时场景
     * @note 应在调度器启动前调用，避免运行时替换导致定时器丢失
     * @example
     * ```cpp
     * // 创建适合短超时场景的时间轮（60秒范围，1秒精度）
     * TimingWheelTimerManager manager(60, 1000000000ULL);
     * scheduler->replaceTimerManager(std::move(manager));
     * ```
     */
    void replaceTimerManager(TimingWheelTimerManager&& manager) {
        m_timer_manager = std::move(manager);
    }


    /** @brief 跨线程唤醒后端的阻塞 poll。 */
    void notify() { m_reactor.notify(); }

    /**
     * @brief 以下注册操作只允许 owner 线程访问；结果原样交回 awaitable。
     * @return 1=立即完成，0=已登记，负数=错误；close/remove 的 0 表示成功。
     */
    int addAccept(IOController* controller) { return m_reactor.addAccept(controller); }  ///< accept；返回后端结果。
    int addConnect(IOController* controller) { return m_reactor.addConnect(controller); }  ///< connect；返回后端结果。
    int addRecv(IOController* controller) { return m_reactor.addRecv(controller); }  ///< recv；返回后端结果。
    int addSend(IOController* controller) { return m_reactor.addSend(controller); }  ///< send；返回后端结果。
    int addReadv(IOController* controller) { return m_reactor.addReadv(controller); }  ///< readv；返回后端结果。
    int addWritev(IOController* controller) { return m_reactor.addWritev(controller); }  ///< writev；返回后端结果。
    int addClose(IOController* controller) { return m_reactor.addClose(controller); }  ///< close；返回后端结果。
    int addFileRead(IOController* controller) { return m_reactor.addFileRead(controller); }  ///< 文件读取；返回后端结果。
    int addFileWrite(IOController* controller) { return m_reactor.addFileWrite(controller); }  ///< 文件写入；返回后端结果。
    int addRecvFrom(IOController* controller) { return m_reactor.addRecvFrom(controller); }  ///< recvfrom；返回后端结果。
    int addSendTo(IOController* controller) { return m_reactor.addSendTo(controller); }  ///< sendto；返回后端结果。
    int addFileWatch(IOController* controller) { return m_reactor.addFileWatch(controller); }  ///< 文件监控；返回后端结果。
    int addSendFile(IOController* controller) { return m_reactor.addSendFile(controller); }  ///< sendfile；返回后端结果。
    int addSequence(IOController* controller) { return m_reactor.addSequence(controller); }  ///< 组合式序列；返回后端结果。
    int remove(IOController* controller) { return m_reactor.remove(controller); }  ///< 移除注册；返回后端结果。

    /** @brief 内部后端最近一次错误；无错误时为空。 */
    std::optional<IOError> lastError() const { return detail::loadBackendError(m_last_error_code); }

    /** @brief 启动前配置 Runtime 借用的 sibling 视图；视图须覆盖整个运行周期。 */
    void configureStealDomain(std::span<IOScheduler* const> siblings, size_t self_index) {
        m_worker.configureStealDomain(self_index, siblings);
    }

    /** @brief 内部窃取队列入口；当前 IO 后端默认禁用窃取。 */
    IOReadyQueue* stealWorkerState() noexcept { return &m_worker; }

    /** @brief 仅在停止或外部同步后读取统计快照。 */
    IOSchedulerStealStats stealStats() const noexcept { return m_worker.snapshotStealStats(); }

protected:
    // 具体调度器析构前先 stop，保证 reactor/队列直到工作线程退出后才析构。
    ~IOSchedulerBase() = default;

    std::expected<void, IOError> startImpl()
    {
        if (m_running.exchange(true, std::memory_order_acq_rel)) { return {}; }
        m_last_error_code.store(0, std::memory_order_release);
        const auto reactor_ready = m_reactor.start();
        if (!reactor_ready) {
            m_running.store(false, std::memory_order_release);
            return std::unexpected(reactor_ready.error());
        }
        if (!m_worker.reopenResumeAdmission()) {
            m_running.store(false, std::memory_order_release);
            return std::unexpected(IOError(kNotReady, 0));
        }
        std::promise<void> thread_ready;
        auto ready = thread_ready.get_future();
        m_thread = std::thread([this, thread_ready = std::move(thread_ready)]() mutable {
            detail::SchedulerThreadScope scheduler_thread_scope;
            this->m_threadId = std::this_thread::get_id();
            thread_ready.set_value();
            // 沿用现有亲和性行为；错误传播由后续亲和性配置改动处理。
            (void)this->applyConfiguredAffinity();
            eventLoop();
        });
        ready.wait();
        return {};
    }

    void stopImpl()
    {
        m_worker.closeResumeAdmission();
        if (!m_running.exchange(false, std::memory_order_acq_rel)) { return; }
        m_wake_coordinator.forceWake([this]() { notify(); });
        if (m_thread.joinable()) { m_thread.join(); }
    }

    bool scheduleImpl(TaskRef task) noexcept
    {
        if (!this->bindTask(task)) { return false; }
        if (std::this_thread::get_id() == this->m_threadId) {
            m_worker.scheduleLocal(std::move(task));
            return true;
        }
        return wakeAfterInjection(m_worker.scheduleInjected(std::move(task)));
    }

    bool scheduleResumeImpl(TaskRef task) noexcept
    {
        if (!this->bindTask(task)) { return false; }
        if (std::this_thread::get_id() == this->m_threadId) {
            m_worker.scheduleLocal(std::move(task));
            return true;
        }
        return wakeAfterInjection(m_worker.scheduleResume(std::move(task)));
    }

    bool scheduleReadyEntryImpl(detail::ReadyEntry& entry) noexcept
    {
        if (!entry.isValid() || detail::readyEntryScheduler(entry) != this) { return false; }
        if (std::this_thread::get_id() == this->m_threadId) {
            m_worker.scheduleLocal(std::move(entry));
            return true;
        }
        return wakeAfterInjection(m_worker.scheduleInjected(std::move(entry)));
    }

    bool scheduleDeferredImpl(TaskRef task) noexcept
    {
        if (!this->bindTask(task)) { return false; }
        if (std::this_thread::get_id() == this->m_threadId) {
            m_worker.scheduleLocalDeferred(std::move(task));
            return true;
        }
        return wakeAfterInjection(m_worker.scheduleInjected(std::move(task)));
    }

    bool scheduleImmediatelyImpl(TaskRef task) noexcept
    {
        if (!this->bindTask(task)) { return false; }
        this->resume(task);
        return true;
    }

    /**
     * @brief 注册定时器
     * @details 添加任务量不大且数量不是特别多的定时任务，如IO超时，sleep等
     * @param timer 定时器共享指针
     * @return true 定时器已加入当前调度器的时间轮；false 插入失败
     */
    bool addTimerImpl(Timer::ptr timer) {
        return m_timer_manager.push(timer);
    }

    /**
     * @brief kqueue 等纳秒 poll 接口的等待超时
     *
     * 空轮使用 idle 上限，非空轮对齐下一个 tick 边界；统一在纳秒域
     * 应用通用毫秒上限，避免 kqueue 发生 ms->ns 往返精度损失。
     */
    uint64_t schedulerPollTimeoutNanoseconds() const noexcept {
        constexpr uint64_t kNsPerMs = 1'000'000ULL;
        const uint64_t max_ns =
            static_cast<uint64_t>(GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS) * kNsPerMs;
        uint64_t ns = m_timer_manager.empty()
            ? static_cast<uint64_t>(GALAY_KERNEL_IO_POLL_IDLE_TIMEOUT_MS) * kNsPerMs
            : m_timer_manager.nsToNextTickBoundary();
        return std::min(max_ns, ns);
    }

    /** @brief epoll 等毫秒接口使用纳秒边界的向上取整结果。 */
    int schedulerPollTimeoutMilliseconds() const noexcept {
        constexpr uint64_t kNsPerMs = 1'000'000ULL;
        const uint64_t ns = schedulerPollTimeoutNanoseconds();
        const uint64_t rounded_ms = (ns + kNsPerMs - 1) / kNsPerMs;
        const uint64_t ms = std::max<uint64_t>(GALAY_KERNEL_IO_POLL_TIMEOUT_MIN_MS,
                                               rounded_ms);
        return static_cast<int>(std::min<uint64_t>(ms, GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS));
    }

    /**
     * @brief io_uring 完成等待的纳秒上限。
     * @details io_uring 的有效等待时间还会受空闲轮 50ms 默认值影响，最终
     *          取时间轮边界与 GALAY_KERNEL_IO_POLL_WAIT_MAX_NS 的较小值。
     */
    uint64_t schedulerPollTimeoutIoUringNanoseconds() const noexcept {
        return std::min<uint64_t>(schedulerPollTimeoutNanoseconds(),
                                  GALAY_KERNEL_IO_POLL_WAIT_MAX_NS);
    }
    IOReadyQueue m_worker;
    TimingWheelTimerManager m_timer_manager{GALAY_KERNEL_TIMER_WHEEL_TICK_NS};
    std::thread m_thread;
    std::atomic<uint64_t> m_last_error_code{0};
    std::atomic<bool> m_sleeping{true};
    std::atomic<bool> m_wakeup_pending{false};
    WakeCoordinator m_wake_coordinator;
    SchedulerCore m_core;
    Reactor m_reactor;
    int m_batch_size;
    std::atomic<bool> m_running{false};

private:
    bool wakeAfterInjection(std::optional<bool> queue_was_empty) noexcept
    {
        if (!queue_was_empty) { return false; }
        // 返回值只表示唤醒是否合并；成功入队的任务始终已被接纳。
        (void)m_wake_coordinator.requestWake(*queue_was_empty, [this]() { notify(); });
        return true;
    }

    void processPendingTasks()
    {
        detail::ioSchedulerProcessPendingTasks(
            m_core, m_wake_coordinator, [this](TaskRef& task) { this->resume(task); });
    }

    void eventLoop()
    {
        auto& backend = static_cast<Derived&>(*this);
        detail::runIOSchedulerEventLoop(
            m_running, m_core, m_timer_manager, m_wake_coordinator,
            static_cast<size_t>(m_batch_size),
            [this](TaskRef& task) { this->resume(task); },
            [&backend]() { backend.pollBackend(); },
            [&backend]() { backend.flushBackend(); });
#ifdef USE_EPOLL
        // Accept 的最小停机闭环；其他 operation 的统一 drain 属于后续阶段。
        // 仍在 owner 上，local resume admission 可完成已接纳的等待者。
        m_reactor.stopAccepts();
        while (m_core.hasPendingWork()) {
            const auto ran = m_core.runReadyPass(
                [this](TaskRef& task) { this->resume(task); },
                [this](size_t drained) { m_wake_coordinator.onRemoteCollected(drained); });
            if (ran == 0) { break; }
            backend.flushBackend();
        }
#endif
#ifdef USE_IOURING
        // io_uring has no readiness registration queue, but its persistent
        // accept resource still needs an owner-side logical stop completion.
        m_reactor.stopAccepts();
        while (m_core.hasPendingWork()) {
            const auto ran = m_core.runReadyPass(
                [this](TaskRef& task) { this->resume(task); },
                [this](size_t drained) { m_wake_coordinator.onRemoteCollected(drained); });
            if (ran == 0) { break; }
        }
#endif
    }
};

} // namespace galay::kernel
#endif
