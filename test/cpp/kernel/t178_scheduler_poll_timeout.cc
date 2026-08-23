/**
 * @file t178_scheduler_poll_timeout.cc
 * @brief 验证不同时间轮 tick 下 poll 等待遵守纳秒上限。
 */

#if defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
#elif defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
#else
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#endif
#include <galay/cpp/galay-kernel/core/timeout.hpp>
#include <cassert>
#include <chrono>
#include <iostream>

using namespace galay::kernel;

namespace {

#if defined(USE_IOURING)
using TestScheduler = IOUringScheduler;
#elif defined(USE_KQUEUE)
using TestScheduler = KqueueScheduler;
#else
using TestScheduler = EpollScheduler;
#endif

struct SchedulerProbe final : TestScheduler {
    using IOScheduler::addTimer;
    using IOScheduler::replaceTimerManager;
    using IOScheduler::schedulerPollTimeoutMilliseconds;
    using IOScheduler::schedulerPollTimeoutNanoseconds;
    using IOScheduler::schedulerPollTimeoutIoUringNanoseconds;
};

void checkTick(uint64_t tick_ns)
{
    SchedulerProbe scheduler;
    scheduler.replaceTimerManager(TimingWheelTimerManager(tick_ns));
    assert(scheduler.schedulerPollTimeoutNanoseconds() <=
           static_cast<uint64_t>(GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS) * 1'000'000ULL);
    assert(scheduler.schedulerPollTimeoutIoUringNanoseconds() <= GALAY_KERNEL_IO_POLL_WAIT_MAX_NS);
    assert(scheduler.schedulerPollTimeoutMilliseconds() <= GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS);

    auto timer = TimeoutTimer::create(std::chrono::seconds(1));
    assert(scheduler.addTimer(timer));
    assert(scheduler.schedulerPollTimeoutNanoseconds() <=
           static_cast<uint64_t>(GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS) * 1'000'000ULL);
    assert(scheduler.schedulerPollTimeoutIoUringNanoseconds() <= GALAY_KERNEL_IO_POLL_WAIT_MAX_NS);
}

void checkSubMillisecondBoundary()
{
    SchedulerProbe scheduler;
    scheduler.replaceTimerManager(TimingWheelTimerManager(500'000ULL));
    auto timer = TimeoutTimer::create(std::chrono::seconds(1));
    assert(scheduler.addTimer(timer));
    assert(scheduler.schedulerPollTimeoutNanoseconds() < 1'000'000ULL);
    assert(scheduler.schedulerPollTimeoutMilliseconds() == 1);
}

void checkBackendSpecificUpperBound()
{
    SchedulerProbe scheduler;
    scheduler.replaceTimerManager(TimingWheelTimerManager(100'000'000'000ULL));
    auto timer = TimeoutTimer::create(std::chrono::seconds(1));
    assert(scheduler.addTimer(timer));
    assert(scheduler.schedulerPollTimeoutMilliseconds() <= GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS);
    assert(scheduler.schedulerPollTimeoutIoUringNanoseconds() <= GALAY_KERNEL_IO_POLL_WAIT_MAX_NS);
}

}  // namespace

int main()
{
    checkTick(50'000'000ULL);
    checkTick(100'000'000ULL);
    checkSubMillisecondBoundary();
    checkBackendSpecificUpperBound();
    std::cout << "T178-SchedulerPollTimeout PASS\n";
    return 0;
}
