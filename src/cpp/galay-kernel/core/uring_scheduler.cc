/**
 * @file uring_scheduler.cc
 * @brief Linux io_uring IO 调度器实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 将 IO 操作委托给 IOUringReactor，复用 SchedulerCore / WakeCoordinator 事件循环机制。
 * 由于 io_uring SQE 获取/提交在调度器内必须保持单线程，跨线程窃取被禁用。
 */

#include "uring_scheduler.h"
#include "sched_loop.hpp"

#ifdef USE_IOURING

namespace galay::kernel
{

IOUringScheduler::IOUringScheduler(int queue_depth, int batch_size)
    : m_running(false)
    , m_queue_depth(queue_depth)
    , m_batch_size(batch_size)
    , m_worker(static_cast<size_t>(batch_size))
    , m_wake_coordinator(m_sleeping, m_wakeup_pending)
    , m_core(m_worker, static_cast<size_t>(batch_size))
    , m_reactor(queue_depth, m_last_error_code)
{
    // io_uring SQE 获取/提交在每个调度器内保持单线程；被窃取的协程
    // 仍可通过其所属 reactor 提交，因此跨线程窃取不安全。
    m_worker.setStealingEnabled(false);
}

IOUringScheduler::~IOUringScheduler()
{
    stop();
}

void IOUringScheduler::start()
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

void IOUringScheduler::stop()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    m_wake_coordinator.forceWake([this]() { notify(); });

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void IOUringScheduler::notify()
{
    m_reactor.notify();
}

int IOUringScheduler::addAccept(IOController* controller)
{
    return m_reactor.addAccept(controller);
}

int IOUringScheduler::addConnect(IOController* controller)
{
    return m_reactor.addConnect(controller);
}

int IOUringScheduler::addRecv(IOController* controller)
{
    return m_reactor.addRecv(controller);
}

int IOUringScheduler::addSend(IOController* controller)
{
    return m_reactor.addSend(controller);
}

int IOUringScheduler::addReadv(IOController* controller)
{
    return m_reactor.addReadv(controller);
}

int IOUringScheduler::addWritev(IOController* controller)
{
    return m_reactor.addWritev(controller);
}

int IOUringScheduler::addClose(IOController* controller)
{
    return m_reactor.addClose(controller);
}

int IOUringScheduler::addFileRead(IOController* controller)
{
    return m_reactor.addFileRead(controller);
}

int IOUringScheduler::addFileWrite(IOController* controller)
{
    return m_reactor.addFileWrite(controller);
}

int IOUringScheduler::addRecvFrom(IOController* controller)
{
    return m_reactor.addRecvFrom(controller);
}

int IOUringScheduler::addSendTo(IOController* controller)
{
    return m_reactor.addSendTo(controller);
}

int IOUringScheduler::addFileWatch(IOController* controller)
{
    return m_reactor.addFileWatch(controller);
}

int IOUringScheduler::addSendFile(IOController* controller)
{
    return m_reactor.addSendFile(controller);
}

int IOUringScheduler::addSequence(IOController* controller)
{
    return m_reactor.addSequence(controller);
}

int IOUringScheduler::remove(IOController* controller)
{
    return m_reactor.remove(controller);
}

std::optional<IOError> IOUringScheduler::lastError() const
{
    return detail::loadBackendError(m_last_error_code);
}

bool IOUringScheduler::schedule(TaskRef task)
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

bool IOUringScheduler::scheduleDeferred(TaskRef task)
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

bool IOUringScheduler::scheduleImmediately(TaskRef task)
{
    if (!bindTask(task)) {
        return false;
    }
    resume(task);
    return true;
}

void IOUringScheduler::processPendingTasks()
{
    detail::ioSchedulerProcessPendingTasks(
        m_core,
        m_wake_coordinator,
        [this](TaskRef& next) { resume(next); });
}

void IOUringScheduler::eventLoop()
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
            const uint64_t wait_ns = detail::halfTickPollWaitNanoseconds(tick_ns);
            m_reactor.poll(wait_ns, m_wake_coordinator);
        },
        []() {});
}

}

#endif // USE_IOURING
