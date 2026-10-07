#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <concepts>
#include <type_traits>

#if defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using IOSchedulerType = galay::kernel::KqueueScheduler;
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using IOSchedulerType = galay::kernel::EpollScheduler;
#elif defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::kernel;

namespace {

Task<void> noop_task() {
    co_return;
}

template <typename SchedulerT>
concept HasTaskScheduleHelpers = requires(SchedulerT& scheduler) {
    { schedule_task(scheduler, noop_task()) } -> std::same_as<bool>;
    { schedule_task_deferred(scheduler, noop_task()) } -> std::same_as<bool>;
    { schedule_task_immediately(scheduler, noop_task()) } -> std::same_as<bool>;
};

static_assert(HasTaskScheduleHelpers<ParallelScheduler>,
              "ParallelScheduler should accept Task helpers without exposing detail::TaskAccess");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
static_assert(HasTaskScheduleHelpers<IOSchedulerType>,
              "IOScheduler should accept Task helpers without exposing detail::TaskAccess");
#endif

}  // namespace

int main() {
    return 0;
}
