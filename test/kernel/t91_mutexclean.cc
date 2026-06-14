/**
 * @file t91_mutexclean.cc
 * @brief 用途：验证 `AsyncMutex` 会在 timeout 路径后正确清理 waiter。
 * 关键覆盖点：超时 waiter 失效、后续 unlock 不应重新占锁、mutex 可被后续任务继续获取。
 * 通过条件：超时任务返回 timeout，持锁任务释放后新的 probe 任务仍能成功获取锁。
 */

#include "kernel/concurrency/async_mutex.h"
#include "kernel/concurrency/async_waiter.h"
#include "kernel/kernel/task.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <thread>

#ifdef USE_EPOLL
#include "kernel/kernel/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include "kernel/kernel/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#elif defined(USE_IOURING)
#include "kernel/kernel/uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_holder_locked{false};
std::atomic<bool> g_holder_unlocked{false};
std::atomic<bool> g_timeout_done{false};
std::atomic<bool> g_timeout_was_timeout{false};
std::atomic<bool> g_probe_done{false};
std::atomic<bool> g_probe_acquired{false};

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

bool waitUntil(const std::atomic<bool>& flag, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

Task<void> holderTask(AsyncMutex* mutex, AsyncWaiter<void>* release_waiter)
{
    auto lock_result = co_await mutex->lock();
    require(lock_result.has_value(), "holder should acquire mutex");

    g_holder_locked.store(true, std::memory_order_release);

    auto release_result = co_await release_waiter->wait();
    require(release_result.has_value(), "holder release signal should succeed");

    mutex->unlock();
    g_holder_unlocked.store(true, std::memory_order_release);
    co_return;
}

Task<void> timeoutWaiterTask(AsyncMutex* mutex)
{
    auto lock_result = co_await mutex->lock().timeout(20ms);
    g_timeout_was_timeout.store(
        !lock_result.has_value() &&
            IOError::contains(lock_result.error().code(), kTimeout),
        std::memory_order_release);
    g_timeout_done.store(true, std::memory_order_release);
    co_return;
}

Task<void> probeTask(AsyncMutex* mutex)
{
    auto lock_result = co_await mutex->lock().timeout(50ms);
    if (lock_result.has_value()) {
        g_probe_acquired.store(true, std::memory_order_release);
        mutex->unlock();
    }
    g_probe_done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    IOSchedulerType scheduler;
    AsyncMutex mutex;
    AsyncWaiter<void> release_waiter;

    scheduler.start();

    scheduler.schedule(detail::TaskAccess::detachTask(holderTask(&mutex, &release_waiter)));
    require(waitUntil(g_holder_locked, 2s), "holder did not acquire mutex in time");

    scheduler.schedule(detail::TaskAccess::detachTask(timeoutWaiterTask(&mutex)));
    require(waitUntil(g_timeout_done, 2s), "timeout waiter did not finish in time");
    require(g_timeout_was_timeout.load(std::memory_order_acquire),
            "timeout waiter should complete with timeout");

    require(release_waiter.notify(), "release waiter should be notified once");
    require(waitUntil(g_holder_unlocked, 2s), "holder did not unlock in time");

    scheduler.schedule(detail::TaskAccess::detachTask(probeTask(&mutex)));
    require(waitUntil(g_probe_done, 2s), "probe task did not finish in time");

    scheduler.stop();

    require(g_probe_acquired.load(std::memory_order_acquire),
            "probe should acquire mutex after timed-out waiter is cleaned up");

    std::cout << "T91-AsyncMutexTimeoutCleanup PASS\n";
    return 0;
}
