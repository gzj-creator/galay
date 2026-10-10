/**
 * @file io_scheduler_base.hpp
 * @brief 三个 IO 后端共用的调度器实现。
 * @details 统一持有线程、就绪队列、时间轮及 reactor；Derived 仅实现
 * poll_backend() 和 flush_backend()，把平台的超时格式及批量提交差异留在后端。
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
        m_worker.set_stealing_enabled(false);
        m_worker.close_resume_admission();
    }

public:
    /**
     * @brief 替换调度器的定时器管理器
     * @param manager 新的时间轮定时器管理器（右值引用）
     * @return 无返回值
     * @details 用于自定义时间轮配置（wheel_size、tick_duration）以适应不同的超时场景
     * @note 应在调度器启动前调用，避免运行时替换导致定时器丢失
     * @example
     * ```cpp
     * // 创建适合短超时场景的时间轮（60秒范围，1秒精度）
     * TimingWheelTimerManager manager(60, 1000000000ULL);
     * scheduler->replace_timer_manager(std::move(manager));
     * ```
     */
    void replace_timer_manager(TimingWheelTimerManager&& manager) {
        m_timer_manager = std::move(manager);
    }


    /**
     * @brief 跨线程唤醒后端的阻塞 poll。
     * @return 无返回值
     */
    void notify() { m_reactor.notify(); }

    /**
     * @brief 以下注册操作只允许 owner 线程访问；结果原样交回 awaitable。
     * @param controller IO 控制器
     * @return 1=立即完成，0=已登记，负数=错误；close/remove 的 0 表示成功。
     */
    /**
     * @brief accept；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_accept(IOController* controller) { return m_reactor.add_accept(controller); }
    /**
     * @brief connect；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_connect(IOController* controller) { return m_reactor.add_connect(controller); }
    /**
     * @brief recv；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_recv(IOController* controller) { return m_reactor.add_recv(controller); }
    /**
     * @brief send；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_send(IOController* controller) { return m_reactor.add_send(controller); }
    /**
     * @brief readv；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_readv(IOController* controller) { return m_reactor.add_readv(controller); }
    /**
     * @brief writev；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_writev(IOController* controller) { return m_reactor.add_writev(controller); }
    /**
     * @brief close；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_close(IOController* controller) { return m_reactor.add_close(controller); }
    /**
     * @brief 文件读取；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_file_read(IOController* controller) { return m_reactor.add_file_read(controller); }
    /**
     * @brief 文件写入；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_file_write(IOController* controller) { return m_reactor.add_file_write(controller); }
    /**
     * @brief recvfrom；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_recv_from(IOController* controller) { return m_reactor.add_recv_from(controller); }
    /**
     * @brief sendto；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_send_to(IOController* controller) { return m_reactor.add_send_to(controller); }
    /**
     * @brief 文件监控；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_file_watch(IOController* controller) { return m_reactor.add_file_watch(controller); }
    /**
     * @brief sendfile；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_send_file(IOController* controller) { return m_reactor.add_send_file(controller); }
    /**
     * @brief 组合式序列；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_sequence(IOController* controller) { return m_reactor.add_sequence(controller); }
    /**
     * @brief 移除注册；返回后端结果。
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int remove(IOController* controller) { return m_reactor.remove(controller); }

    /**
     * @brief 内部后端最近一次错误；无错误时为空。
     * @return std::optional<IOError> 结果，含义见函数说明
     */
    std::optional<IOError> last_error() const { return detail::load_backend_error(m_last_error_code); }

    /**
     * @brief 启动前配置 Runtime 借用的 sibling 视图；视图须覆盖整个运行周期。
     * @param siblings 同级调度器集合
     * @param self_index 当前调度器索引
     * @return 无返回值
     */
    void configure_steal_domain(std::span<IOScheduler* const> siblings, size_t self_index) {
        m_worker.configure_steal_domain(self_index, siblings);
    }

    /**
     * @brief 内部窃取队列入口；当前 IO 后端默认禁用窃取。
     * @return IOReadyQueue* 指针
     */
    IOReadyQueue* steal_worker_state() noexcept { return &m_worker; }

    /**
     * @brief 仅在停止或外部同步后读取统计快照。
     * @return 当前调度器的工作窃取统计快照
     */
    IOSchedulerStealStats steal_stats() const noexcept { return m_worker.snapshot_steal_stats(); }

protected:
    // 具体调度器析构前先 stop，保证 reactor/队列直到工作线程退出后才析构。
    ~IOSchedulerBase() = default;

    std::expected<void, IOError> start_impl()
    {
        if (m_running.exchange(true, std::memory_order_acq_rel)) { return {}; }
        m_last_error_code.store(0, std::memory_order_release);
        const auto reactor_ready = m_reactor.start();
        if (!reactor_ready) {
            m_running.store(false, std::memory_order_release);
            return std::unexpected(reactor_ready.error());
        }
        if (!m_worker.reopen_resume_admission()) {
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
            (void)this->apply_configured_affinity();
            event_loop();
        });
        ready.wait();
        return {};
    }

    void stop_impl()
    {
        m_worker.close_resume_admission();
        if (!m_running.exchange(false, std::memory_order_acq_rel)) { return; }
        m_wake_coordinator.force_wake([this]() { notify(); });
        if (m_thread.joinable()) { m_thread.join(); }
    }

    bool schedule_impl(TaskRef task) noexcept
    {
        if (!this->bind_task(task)) { return false; }
        if (std::this_thread::get_id() == this->m_threadId) {
            m_worker.schedule_local(std::move(task));
            return true;
        }
        return wake_after_injection(m_worker.schedule_injected(std::move(task)));
    }

    bool schedule_resume_impl(TaskRef task) noexcept
    {
        if (!this->bind_task(task)) { return false; }
        if (std::this_thread::get_id() == this->m_threadId) {
            m_worker.schedule_local(std::move(task));
            return true;
        }
        return wake_after_injection(m_worker.schedule_resume(std::move(task)));
    }

    bool schedule_ready_entry_impl(detail::ReadyEntry& entry) noexcept
    {
        if (!entry.is_valid() || detail::ready_entry_scheduler(entry) != this) { return false; }
        if (std::this_thread::get_id() == this->m_threadId) {
            m_worker.schedule_local(std::move(entry));
            return true;
        }
        return wake_after_injection(m_worker.schedule_injected(std::move(entry)));
    }

    bool schedule_deferred_impl(TaskRef task) noexcept
    {
        if (!this->bind_task(task)) { return false; }
        if (std::this_thread::get_id() == this->m_threadId) {
            m_worker.schedule_local_deferred(std::move(task));
            return true;
        }
        return wake_after_injection(m_worker.schedule_injected(std::move(task)));
    }

    bool schedule_immediately_impl(TaskRef task) noexcept
    {
        if (!this->bind_task(task)) { return false; }
        this->resume(task);
        return true;
    }

    /**
     * @brief 注册定时器
     * @details 添加任务量不大且数量不是特别多的定时任务，如IO超时，sleep等
     * @param timer 定时器共享指针
     * @return true 定时器已加入当前调度器的时间轮；false 插入失败
     */
    bool add_timer_impl(Timer::ptr timer) {
        return m_timer_manager.push(timer);
    }

    /**
     * @brief kqueue 等纳秒 poll 接口的等待超时
     *
     * 空轮使用 idle 上限，非空轮对齐下一个 tick 边界；统一在纳秒域
     * 应用通用毫秒上限，避免 kqueue 发生 ms->ns 往返精度损失。
     * @return 轮询等待上限，单位为纳秒
     */
    uint64_t scheduler_poll_timeout_nanoseconds() const noexcept {
        constexpr uint64_t kNsPerMs = 1'000'000ULL;
        const uint64_t max_ns =
            static_cast<uint64_t>(GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS) * kNsPerMs;
        uint64_t ns = m_timer_manager.empty()
            ? static_cast<uint64_t>(GALAY_KERNEL_IO_POLL_IDLE_TIMEOUT_MS) * kNsPerMs
            : m_timer_manager.ns_to_next_tick_boundary();
        return std::min(max_ns, ns);
    }

    /**
     * @brief epoll 等毫秒接口使用纳秒边界的向上取整结果。
     * @return 向上取整的轮询等待上限，单位为毫秒
     */
    int scheduler_poll_timeout_milliseconds() const noexcept {
        constexpr uint64_t kNsPerMs = 1'000'000ULL;
        const uint64_t ns = scheduler_poll_timeout_nanoseconds();
        const uint64_t rounded_ms = (ns + kNsPerMs - 1) / kNsPerMs;
        const uint64_t ms = std::max<uint64_t>(GALAY_KERNEL_IO_POLL_TIMEOUT_MIN_MS,
                                               rounded_ms);
        return static_cast<int>(std::min<uint64_t>(ms, GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS));
    }

    /**
     * @brief io_uring 完成等待的纳秒上限。
     * @return io_uring 完成等待上限，单位为纳秒
     * @details io_uring 的有效等待时间还会受空闲轮 50ms 默认值影响，最终
     *          取时间轮边界与 GALAY_KERNEL_IO_POLL_WAIT_MAX_NS 的较小值。
     */
    uint64_t scheduler_poll_timeout_io_uring_nanoseconds() const noexcept {
        return std::min<uint64_t>(scheduler_poll_timeout_nanoseconds(),
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
    bool wake_after_injection(std::optional<bool> queue_was_empty) noexcept
    {
        if (!queue_was_empty) { return false; }
        // 返回值只表示唤醒是否合并；成功入队的任务始终已被接纳。
        (void)m_wake_coordinator.request_wake(*queue_was_empty, [this]() { notify(); });
        return true;
    }

    void process_pending_tasks()
    {
        detail::io_scheduler_process_pending_tasks(
            m_core, m_wake_coordinator, [this](TaskRef& task) { this->resume(task); });
    }

    void event_loop()
    {
        auto& backend = static_cast<Derived&>(*this);
        detail::run_io_scheduler_event_loop(
            m_running, m_core, m_timer_manager, m_wake_coordinator,
            static_cast<size_t>(m_batch_size),
            [this](TaskRef& task) { this->resume(task); },
            [&backend]() { backend.poll_backend(); },
            [&backend]() { backend.flush_backend(); });
#ifdef USE_EPOLL
        // Accept 的最小停机闭环；其他 operation 的统一 drain 属于后续阶段。
        // 仍在 owner 上，local resume admission 可完成已接纳的等待者。
        m_reactor.stop_accepts();
        while (m_core.has_pending_work()) {
            const auto ran = m_core.run_ready_pass(
                [this](TaskRef& task) { this->resume(task); },
                [this](size_t drained) { m_wake_coordinator.on_remote_collected(drained); });
            if (ran == 0) { break; }
            backend.flush_backend();
        }
#endif
#ifdef USE_IOURING
        // io_uring has no readiness registration queue, but its persistent
        // accept resource still needs an owner-side logical stop completion.
        m_reactor.stop_accepts();
        while (m_core.has_pending_work()) {
            const auto ran = m_core.run_ready_pass(
                [this](TaskRef& task) { this->resume(task); },
                [this](size_t drained) { m_wake_coordinator.on_remote_collected(drained); });
            if (ran == 0) { break; }
        }
#endif
    }
};

} // namespace galay::kernel
#endif
