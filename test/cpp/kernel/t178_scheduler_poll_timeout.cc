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
    using IOScheduler::add_timer;
    using IOScheduler::replace_timer_manager;
    using IOScheduler::scheduler_poll_timeout_milliseconds;
    using IOScheduler::scheduler_poll_timeout_nanoseconds;
    using IOScheduler::scheduler_poll_timeout_io_uring_nanoseconds;
};

void check_tick(uint64_t tick_ns)
{
    SchedulerProbe scheduler;
    scheduler.replace_timer_manager(TimingWheelTimerManager(tick_ns));
    assert(scheduler.scheduler_poll_timeout_nanoseconds() <=
           static_cast<uint64_t>(GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS) * 1'000'000ULL);
    assert(scheduler.scheduler_poll_timeout_io_uring_nanoseconds() <= GALAY_KERNEL_IO_POLL_WAIT_MAX_NS);
    assert(scheduler.scheduler_poll_timeout_milliseconds() <= GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS);

    auto timer = TimeoutTimer::create(std::chrono::seconds(1));
    assert(scheduler.add_timer(timer));
    assert(scheduler.scheduler_poll_timeout_nanoseconds() <=
           static_cast<uint64_t>(GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS) * 1'000'000ULL);
    assert(scheduler.scheduler_poll_timeout_io_uring_nanoseconds() <= GALAY_KERNEL_IO_POLL_WAIT_MAX_NS);
}

void check_sub_millisecond_boundary()
{
    SchedulerProbe scheduler;
    scheduler.replace_timer_manager(TimingWheelTimerManager(500'000ULL));
    auto timer = TimeoutTimer::create(std::chrono::seconds(1));
    assert(scheduler.add_timer(timer));
    assert(scheduler.scheduler_poll_timeout_nanoseconds() < 1'000'000ULL);
    assert(scheduler.scheduler_poll_timeout_milliseconds() == 1);
}

void check_backend_specific_upper_bound()
{
    SchedulerProbe scheduler;
    scheduler.replace_timer_manager(TimingWheelTimerManager(100'000'000'000ULL));
    auto timer = TimeoutTimer::create(std::chrono::seconds(1));
    assert(scheduler.add_timer(timer));
    assert(scheduler.scheduler_poll_timeout_milliseconds() <= GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS);
    assert(scheduler.scheduler_poll_timeout_io_uring_nanoseconds() <= GALAY_KERNEL_IO_POLL_WAIT_MAX_NS);
}

}  // namespace

int main()
{
    check_tick(50'000'000ULL);
    check_tick(100'000'000ULL);
    check_sub_millisecond_boundary();
    check_backend_specific_upper_bound();
    std::cout << "T178-SchedulerPollTimeout PASS\n";
    return 0;
}
