/** @brief 调度器静态分派、类型边界及 Runtime 所有权契约。 */
#include <galay/cpp/galay-kernel/core/runtime.h>
#if defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
#elif defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
#else
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#endif
#include <cassert>
#include <iostream>
#include <type_traits>
#include <utility>

using namespace galay::kernel;

template <typename T>
concept CustomSchedulerInjection = requires(T& runtime) {
    runtime.addIOScheduler(std::unique_ptr<IOScheduler>{});
};

template <typename T>
concept CustomComputeInjection = requires(T& runtime) {
    runtime.addParallelScheduler(std::unique_ptr<ParallelScheduler>{});
};

static_assert(!std::is_polymorphic_v<Scheduler>);
static_assert(!std::is_polymorphic_v<IOScheduler>);
static_assert(!std::is_polymorphic_v<ParallelScheduler>);
static_assert(!std::is_destructible_v<Scheduler>, "统一指针仅借用，不允许通过基类删除");
static_assert(!CustomSchedulerInjection<Runtime>);
static_assert(!CustomComputeInjection<Runtime>);
static_assert(!std::is_default_constructible_v<SchedulerBase<ParallelScheduler, kParallelScheduler>>);

Task<int> answer() { co_return 42; }

namespace {

struct FrameLifetime {
    int* destroyed;
    explicit FrameLifetime(int& count) : destroyed(&count) {}
    FrameLifetime(const FrameLifetime&) = delete;
    FrameLifetime(FrameLifetime&& other) noexcept
        : destroyed(std::exchange(other.destroyed, nullptr)) {}
    ~FrameLifetime() { if (destroyed) { ++*destroyed; } }
};

Task<void> pending(FrameLifetime lifetime)
{
    (void)lifetime;
    co_return;
}

void verifyTaskRefOwnership()
{
    int destroyed = 0;
    TaskRef owner = detail::TaskAccess::detachTask(pending(FrameLifetime(destroyed)));
    auto* state = owner.state();
    assert(state && state->m_refs.load() == 1);
    TaskRef moved(std::move(owner));
    assert(!owner.isValid() && moved.state() == state && state->m_refs.load() == 1);
    TaskRef copy(moved);
    assert(state->m_refs.load() == 2);
    owner = std::move(copy);
    assert(!copy.isValid() && state->m_refs.load() == 2);
    owner = TaskRef{};
    assert(state->m_refs.load() == 1 && destroyed == 0);

    auto handle = std::coroutine_handle<TaskPromise<void>>::from_address(state->m_handle.address());
    const TaskRef& borrowed = handle.promise().taskRefView();
    {
        TaskRef retained(borrowed);
        assert(retained.state() == state && state->m_refs.load() == 2);
    }
    moved = borrowed;
    assert(moved.state() == state && state->m_refs.load() == 1 && destroyed == 0);
    // Moving the mutable promise view onto its sole owner must not destroy
    // the coroutine frame containing the source reference.
    moved = std::move(const_cast<TaskRef&>(borrowed));
    assert(!borrowed.isValid() && moved.state() == state && state->m_refs.load() == 1);
    TaskRef& alias = moved;
    moved = std::move(alias);
    moved = alias;
    assert(moved.state() == state && state->m_refs.load() == 1);
    moved = TaskRef{};
    assert(destroyed == 1);

    TaskRef empty;
    TaskRef empty_copy(empty);
    TaskRef empty_move(std::move(empty));
    assert(!empty_copy.isValid() && !empty_move.isValid());
    TaskRef first = detail::TaskAccess::detachTask(pending(FrameLifetime(destroyed)));
    TaskRef second = detail::TaskAccess::detachTask(pending(FrameLifetime(destroyed)));
    first = std::move(second);
    assert(!second.isValid() && destroyed == 2);
    first = TaskRef{};
    assert(destroyed == 3);
}

template <typename SchedulerT>
void verifyRejectedTasks(SchedulerT& scheduler, Scheduler* other)
{
    assert(!scheduler.schedule(TaskRef{}));
    assert(!scheduler.scheduleResume(TaskRef{}));
    assert(!scheduler.scheduleDeferred(TaskRef{}));
    assert(!scheduler.scheduleImmediately(TaskRef{}));
    TaskRef task = detail::TaskAccess::detachTask(answer());
    detail::setTaskScheduler(task, other);
    assert(!scheduler.schedule(task));
    assert(!scheduler.scheduleResume(task));
    assert(!scheduler.scheduleDeferred(task));
    assert(!scheduler.scheduleImmediately(task));
    assert(task.belongScheduler() == other && task.state()->m_refs.load() == 1);
}

void verifyMissingSchedulers()
{
    auto runtime = RuntimeBuilder().ioSchedulerCount(0).parallelSchedulerCount(0).build();
    const auto io = runtime.blockOnIO(answer());
    const auto cpu = runtime.spawnCpu(answer());
    assert(!io && io.error().code() == RuntimeErrorCode::kNoSchedulerAvailable);
    assert(!cpu && cpu.error().code() == RuntimeErrorCode::kNoSchedulerAvailable);
}

} // namespace

int main()
{
    verifyTaskRefOwnership();
    verifyMissingSchedulers();
    auto runtime = RuntimeBuilder().ioSchedulerCount(1).parallelSchedulerCount(1).build();
    const auto started = runtime.start();
    assert(started);
    Scheduler* io = runtime.getIOScheduler(0);
    Scheduler* compute = runtime.getParallelScheduler(0);
    assert(io && compute && io->type() == kIOScheduler && compute->type() == kParallelScheduler);
    assert(!io->schedule(TaskRef{}));
    assert(!compute->scheduleResume(TaskRef{}));
    detail::ReadyEntry empty;
    assert(!compute->scheduleReadyEntry(empty));
    const auto result = runtime.blockOnIO(answer());
    assert(result && *result == 42);
    const auto computed = runtime.blockOnCpu(answer());
    assert(computed && *computed == 42);
    verifyRejectedTasks(*runtime.getIOScheduler(0), compute);
    verifyRejectedTasks(*runtime.getParallelScheduler(0), io);
    verifyRejectedTasks(*io, compute);
    verifyRejectedTasks(*compute, io);
    auto spawned_io = runtime.spawnIO(answer());
    auto spawned_cpu = runtime.spawnCpu(answer());
    assert(spawned_io && spawned_cpu);
    const auto joined_io = spawned_io->join();
    const auto joined_cpu = spawned_cpu->join();
    assert(joined_io && *joined_io == 42 && joined_cpu && *joined_cpu == 42);
    const auto invalid = runtime.spawnCpu(Task<int>{});
    assert(!invalid && invalid.error().code() == RuntimeErrorCode::kSubmitFailed);
    runtime.stop();
    const auto after_restart = runtime.blockOnCpu(answer());
    assert(after_restart && *after_restart == 42);
    runtime.stop();

    const auto restarted_io = io->start();
    const auto restarted_compute = compute->start();
    assert(restarted_io && restarted_compute);
    io->stop();
    compute->stop();
    std::cout << "T183-SchedulerStaticDispatch PASS\n";
}
