#include <concepts>
#include <expected>
#include <utility>

#include <galay/cpp/galay-kernel/core/compute_scheduler.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/scheduler.hpp>

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#elif defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
#elif defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
#endif

using galay::kernel::ComputeScheduler;
using galay::kernel::Runtime;
using galay::kernel::RuntimeError;
using galay::kernel::Scheduler;
using galay::kernel::Task;
using galay::kernel::TaskRef;

template <typename S>
concept HasTaskRefScheduleSurface = std::derived_from<S, Scheduler> &&
    requires(S& scheduler, TaskRef task) {
        { scheduler.schedule(task) } -> std::same_as<bool>;
        { scheduler.scheduleDeferred(task) } -> std::same_as<bool>;
        { scheduler.scheduleImmediately(task) } -> std::same_as<bool>;
    };

static_assert(HasTaskRefScheduleSurface<ComputeScheduler>,
              "Public scheduler headers should expose TaskRef-native scheduling");
static_assert(requires(Runtime runtime, Task<int> task) {
    { runtime.blockOn(std::move(task)) } -> std::same_as<std::expected<int, RuntimeError>>;
}, "Runtime should expose expected-returning Task-native blockOn");

int main()
{
    return 0;
}
