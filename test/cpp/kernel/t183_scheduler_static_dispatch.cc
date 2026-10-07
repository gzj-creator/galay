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

void verify_task_ref_ownership()
{
    int destroyed = 0;
    TaskRef owner = detail::TaskAccess::detach_task(pending(FrameLifetime(destroyed)));
    auto* state = owner.state();
    assert(state && state->m_refs.load() == 1);
    TaskRef moved(std::move(owner));
    assert(!owner.is_valid() && moved.state() == state && state->m_refs.load() == 1);
    TaskRef copy(moved);
    assert(state->m_refs.load() == 2);
    owner = std::move(copy);
    assert(!copy.is_valid() && state->m_refs.load() == 2);
    owner = TaskRef{};
    assert(state->m_refs.load() == 1 && destroyed == 0);

    auto handle = std::coroutine_handle<TaskPromise<void>>::from_address(state->m_handle.address());
    const TaskRef& borrowed = handle.promise().task_ref_view();
    {
        TaskRef retained(borrowed);
        assert(retained.state() == state && state->m_refs.load() == 2);
    }
    moved = borrowed;
    assert(moved.state() == state && state->m_refs.load() == 1 && destroyed == 0);
    // Moving the mutable promise view onto its sole owner must not destroy
    // the coroutine frame containing the source reference.
    moved = std::move(const_cast<TaskRef&>(borrowed));
    assert(!borrowed.is_valid() && moved.state() == state && state->m_refs.load() == 1);
    TaskRef& alias = moved;
    moved = std::move(alias);
    moved = alias;
    assert(moved.state() == state && state->m_refs.load() == 1);
    moved = TaskRef{};
    assert(destroyed == 1);

    TaskRef empty;
    TaskRef empty_copy(empty);
    TaskRef empty_move(std::move(empty));
    assert(!empty_copy.is_valid() && !empty_move.is_valid());
    TaskRef first = detail::TaskAccess::detach_task(pending(FrameLifetime(destroyed)));
    TaskRef second = detail::TaskAccess::detach_task(pending(FrameLifetime(destroyed)));
    first = std::move(second);
    assert(!second.is_valid() && destroyed == 2);
    first = TaskRef{};
    assert(destroyed == 3);
}

template <typename SchedulerT>
void verify_rejected_tasks(SchedulerT& scheduler, Scheduler* other)
{
    assert(!scheduler.schedule(TaskRef{}));
    assert(!scheduler.schedule_resume(TaskRef{}));
    assert(!scheduler.schedule_deferred(TaskRef{}));
    assert(!scheduler.schedule_immediately(TaskRef{}));
    TaskRef task = detail::TaskAccess::detach_task(answer());
    detail::set_task_scheduler(task, other);
    assert(!scheduler.schedule(task));
    assert(!scheduler.schedule_resume(task));
    assert(!scheduler.schedule_deferred(task));
    assert(!scheduler.schedule_immediately(task));
    assert(task.belong_scheduler() == other && task.state()->m_refs.load() == 1);
}

void verify_missing_schedulers()
{
    auto runtime = RuntimeBuilder().io_scheduler_count(0).parallel_scheduler_count(0).build();
    const auto io = runtime.block_on_io(answer());
    const auto cpu = runtime.spawn_cpu(answer());
    assert(!io && io.error().code() == RuntimeErrorCode::kNoSchedulerAvailable);
    assert(!cpu && cpu.error().code() == RuntimeErrorCode::kNoSchedulerAvailable);
}

} // namespace

int main()
{
    verify_task_ref_ownership();
    verify_missing_schedulers();
    auto runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(1).build();
    const auto started = runtime.start();
    assert(started);
    Scheduler* io = runtime.get_io_scheduler(0);
    Scheduler* compute = runtime.get_parallel_scheduler(0);
    assert(io && compute && io->type() == kIOScheduler && compute->type() == kParallelScheduler);
    assert(!io->schedule(TaskRef{}));
    assert(!compute->schedule_resume(TaskRef{}));
    detail::ReadyEntry empty;
    assert(!compute->schedule_ready_entry(empty));
    const auto result = runtime.block_on_io(answer());
    assert(result && *result == 42);
    const auto computed = runtime.block_on_cpu(answer());
    assert(computed && *computed == 42);
    verify_rejected_tasks(*runtime.get_io_scheduler(0), compute);
    verify_rejected_tasks(*runtime.get_parallel_scheduler(0), io);
    verify_rejected_tasks(*io, compute);
    verify_rejected_tasks(*compute, io);
    auto spawned_io = runtime.spawn_io(answer());
    auto spawned_cpu = runtime.spawn_cpu(answer());
    assert(spawned_io && spawned_cpu);
    const auto joined_io = spawned_io->join();
    const auto joined_cpu = spawned_cpu->join();
    assert(joined_io && *joined_io == 42 && joined_cpu && *joined_cpu == 42);
    const auto invalid = runtime.spawn_cpu(Task<int>{});
    assert(!invalid && invalid.error().code() == RuntimeErrorCode::kSubmitFailed);
    runtime.stop();
    const auto after_restart = runtime.block_on_cpu(answer());
    assert(after_restart && *after_restart == 42);
    runtime.stop();

    const auto restarted_io = io->start();
    const auto restarted_compute = compute->start();
    assert(restarted_io && restarted_compute);
    io->stop();
    compute->stop();
    std::cout << "T183-SchedulerStaticDispatch PASS\n";
}
