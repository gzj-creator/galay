/**
 * @file epoll_scheduler.cc
 * @brief Linux epoll IO 调度器实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 将 IO 操作委托给 EpollReactor，复用 SchedulerCore / WakeCoordinator 事件循环机制。
 */

#include "epoll_scheduler.h"
#include "sched_loop.hpp"

#ifdef USE_EPOLL

namespace galay::kernel
{

EpollScheduler::EpollScheduler(int max_events, int batch_size)
    : m_running(false)
    , m_max_events(max_events)
    , m_batch_size(batch_size)
    , m_worker(static_cast<size_t>(batch_size))
    , m_wake_coordinator(m_sleeping, m_wakeup_pending)
    , m_core(m_worker, static_cast<size_t>(batch_size))
    , m_reactor(max_events, m_last_error_code)
{
    // epoll reactor 注册和删除事件必须保持 owner 线程亲和；被窃取的
    // IO 协程仍会通过其所属 scheduler 的 reactor 提交，跨线程执行不安全。
    m_worker.setStealingEnabled(false);
}

EpollScheduler::~EpollScheduler()
{
    stop();
}

void EpollScheduler::start()
{
    if (m_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    m_last_error_code.store(0, std::memory_order_release);

    m_thread = std::thread([this]() {
        m_threadId = std::this_thread::get_id();
        (void)applyConfiguredAffinity();
        eventLoop();
    });
}

void EpollScheduler::stop()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    m_wake_coordinator.forceWake([this]() { notify(); });

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void EpollScheduler::notify()
{
    m_reactor.notify();
}

int EpollScheduler::addAccept(IOController* controller)
{
    return m_reactor.addAccept(controller);
}

int EpollScheduler::addConnect(IOController* controller)
{
    return m_reactor.addConnect(controller);
}

int EpollScheduler::addRecv(IOController* controller)
{
    return m_reactor.addRecv(controller);
}

int EpollScheduler::addSend(IOController* controller)
{
    return m_reactor.addSend(controller);
}

int EpollScheduler::addReadv(IOController* controller)
{
    return m_reactor.addReadv(controller);
}

int EpollScheduler::addWritev(IOController* controller)
{
    return m_reactor.addWritev(controller);
}

int EpollScheduler::addClose(IOController* controller)
{
    return m_reactor.addClose(controller);
}

int EpollScheduler::addFileRead(IOController* controller)
{
    return m_reactor.addFileRead(controller);
}

int EpollScheduler::addFileWrite(IOController* controller)
{
    return m_reactor.addFileWrite(controller);
}

int EpollScheduler::addRecvFrom(IOController* controller)
{
    return m_reactor.addRecvFrom(controller);
}

int EpollScheduler::addSendTo(IOController* controller)
{
    return m_reactor.addSendTo(controller);
}

int EpollScheduler::addFileWatch(IOController* controller)
{
    return m_reactor.addFileWatch(controller);
}

int EpollScheduler::addSendFile(IOController* controller)
{
    return m_reactor.addSendFile(controller);
}

int EpollScheduler::addSequence(IOController* controller)
{
    return m_reactor.addSequence(controller);
}

int EpollScheduler::remove(IOController* controller)
{
    return m_reactor.remove(controller);
}

std::optional<IOError> EpollScheduler::lastError() const
{
    return detail::loadBackendError(m_last_error_code);
}

bool EpollScheduler::schedule(TaskRef task)
{
    if (!bindTask(task)) {
        return false;
    }

    if (std::this_thread::get_id() == m_threadId) {
        m_worker.scheduleLocal(std::move(task));
        return true;
    }

    const auto queue_was_empty = m_worker.scheduleInjected(std::move(task));
    if (!queue_was_empty.has_value()) {
        return false;
    }
    m_wake_coordinator.requestWake(*queue_was_empty, [this]() { notify(); });
    return true;
}

bool EpollScheduler::scheduleDeferred(TaskRef task)
{
    if (!bindTask(task)) {
        return false;
    }

    if (std::this_thread::get_id() == m_threadId) {
        m_worker.scheduleLocalDeferred(std::move(task));
        return true;
    }

    const auto queue_was_empty = m_worker.scheduleInjected(std::move(task));
    if (!queue_was_empty.has_value()) {
        return false;
    }
    m_wake_coordinator.requestWake(*queue_was_empty, [this]() { notify(); });
    return true;
}

bool EpollScheduler::scheduleImmediately(TaskRef task)
{
    if (!bindTask(task)) {
        return false;
    }
    resume(task);
    return true;
}

void EpollScheduler::processPendingTasks()
{
    detail::ioSchedulerProcessPendingTasks(
        m_core,
        m_wake_coordinator,
        [this](TaskRef& next) { resume(next); });
}

void EpollScheduler::eventLoop()
{
    detail::runIOSchedulerEventLoop(
        m_running,
        m_core,
        m_timer_manager,
        m_wake_coordinator,
        static_cast<size_t>(m_batch_size),
        [this](TaskRef& next) { resume(next); },
        [this]() {
            const uint64_t tick_ns = m_timer_manager.during();
            const int timeout_ms = detail::halfTickPollTimeoutMilliseconds(tick_ns);
            m_reactor.poll(timeout_ms, m_wake_coordinator);
        },
        [this]() { (void)m_reactor.flushPendingChanges(); });
}

}

#endif // USE_EPOLL
