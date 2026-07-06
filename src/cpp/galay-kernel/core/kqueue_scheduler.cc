/**
 * @file kqueue_scheduler.cc
 * @brief macOS/BSD kqueue IO 调度器实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 将 IO 操作委托给 KqueueReactor，复用 SchedulerCore / WakeCoordinator 事件循环机制。
 */

#include "kqueue_scheduler.h"
#include "sched_loop.hpp"
#include "../common/defn.hpp"
#include "awaitable.h"
#include "io_controller.hpp"

#ifdef USE_KQUEUE

#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <future>

namespace galay::kernel
{

KqueueScheduler::KqueueScheduler(int max_events, int batch_size)
    : m_worker(static_cast<size_t>(batch_size))
    , m_sleeping(true)
    , m_wakeup_pending(false)
    , m_wake_coordinator(m_sleeping, m_wakeup_pending)
    , m_core(m_worker, static_cast<size_t>(batch_size))
    , m_reactor(max_events, m_last_error_code)
    , m_max_events(max_events)
    , m_batch_size(batch_size)
    , m_running(false)
{
    // kqueue reactor 注册和删除事件必须保持 owner 线程亲和；被窃取的
    // IO 协程仍会通过其所属 scheduler 的 reactor 提交，跨线程执行不安全。
    m_worker.setStealingEnabled(false);
}

KqueueScheduler::~KqueueScheduler()
{
    KqueueScheduler::stop();
}

std::expected<void, IOError> KqueueScheduler::start()
{
    if (m_running.exchange(true, std::memory_order_acq_rel)) {
        return {}; // 已经在运行
    }
    m_last_error_code.store(0, std::memory_order_release);
    if (auto reactor_ready = m_reactor.start(); !reactor_ready) {
        m_running.store(false, std::memory_order_release);
        return std::unexpected(reactor_ready.error());
    }

    std::promise<void> thread_ready;
    auto ready = thread_ready.get_future();
    m_thread = std::thread([this, thread_ready = std::move(thread_ready)]() mutable {
        detail::SchedulerThreadScope scheduler_thread_scope;
        m_threadId = std::this_thread::get_id();  // 设置调度器线程ID
        thread_ready.set_value();
        (void)applyConfiguredAffinity();
        eventLoop();
    });
    ready.wait();
    return {};
}

void KqueueScheduler::stop()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return; // 已经停止
    }

    m_wake_coordinator.forceWake([this]() { notify(); });

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void KqueueScheduler::notify()
{
    m_reactor.notify();
}

int KqueueScheduler::addAccept(IOController* controller)
{
    return m_reactor.addAccept(controller);
}

int KqueueScheduler::addConnect(IOController* controller)
{
    return m_reactor.addConnect(controller);
}

int KqueueScheduler::addRecv(IOController* controller)
{
    return m_reactor.addRecv(controller);
}

int KqueueScheduler::addSend(IOController* controller)
{
    return m_reactor.addSend(controller);
}

int KqueueScheduler::addReadv(IOController* controller)
{
    return m_reactor.addReadv(controller);
}

int KqueueScheduler::addWritev(IOController* controller)
{
    return m_reactor.addWritev(controller);
}

int KqueueScheduler::addClose(IOController* contoller)
{
    return m_reactor.addClose(contoller);
}

int KqueueScheduler::addFileRead(IOController* controller)
{
    return m_reactor.addFileRead(controller);
}

int KqueueScheduler::addFileWrite(IOController* controller)
{
    return m_reactor.addFileWrite(controller);
}

int KqueueScheduler::addSendFile(IOController* controller)
{
    return m_reactor.addSendFile(controller);
}

int KqueueScheduler::addSequence(IOController* controller)
{
    return m_reactor.addSequence(controller);
}

int KqueueScheduler::remove(IOController* controller)
{
    return m_reactor.remove(controller);
}

std::optional<IOError> KqueueScheduler::lastError() const
{
    return detail::loadBackendError(m_last_error_code);
}

bool KqueueScheduler::schedule(TaskRef task)
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

bool KqueueScheduler::scheduleReadyEntry(detail::ReadyEntry& entry)
{
    if (!entry.isValid() || detail::readyEntryScheduler(entry) != this) {
        return false;
    }

    if (std::this_thread::get_id() == m_threadId) {
        m_worker.scheduleLocal(std::move(entry));
        return true;
    }

    const auto queue_was_empty = m_worker.scheduleInjected(std::move(entry));
    if (!queue_was_empty.has_value()) {
        return false;
    }
    m_wake_coordinator.requestWake(*queue_was_empty, [this]() { notify(); });
    return true;
}

bool KqueueScheduler::scheduleDeferred(TaskRef task)
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

bool KqueueScheduler::scheduleImmediately(TaskRef task)
{
    if (!bindTask(task)) {
        return false;
    }
    resume(task);
    return true;
}

void KqueueScheduler::processPendingTasks()
{
    detail::ioSchedulerProcessPendingTasks(
        m_core,
        m_wake_coordinator,
        [this](TaskRef& next) { resume(next); });
}

void KqueueScheduler::eventLoop()
{
    detail::runIOSchedulerEventLoop(
        m_running,
        m_core,
        m_timer_manager,
        m_wake_coordinator,
        static_cast<size_t>(m_batch_size),
        [this](TaskRef& next) { resume(next); },
        [this]() {
            struct timespec timeout;
            detail::fillTimespecHalfTick(timeout, m_timer_manager.during());
            m_reactor.poll(timeout, m_wake_coordinator);
        },
        [this]() { (void)m_reactor.flushPendingChanges(); });
}

int KqueueScheduler::addRecvFrom(IOController* controller)
{
    return m_reactor.addRecvFrom(controller);
}

int KqueueScheduler::addSendTo(IOController* controller)
{
    return m_reactor.addSendTo(controller);
}

int KqueueScheduler::addFileWatch(IOController* controller)
{
    return m_reactor.addFileWatch(controller);
}


}

#endif // USE_KQUEUE
