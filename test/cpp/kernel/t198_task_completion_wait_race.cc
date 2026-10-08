#include <galay/cpp/galay-kernel/core/runtime.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (condition) return;
    if (std::fprintf(stderr, "%s\n", message) < 0) std::abort();
    std::exit(1);
}

galay::kernel::Task<void> complete_immediately()
{
    co_return;
}

galay::kernel::Task<std::array<int, 16>> complete_with_result()
{
    std::array<int, 16> result{};
    result.fill(42);
    co_return result;
}

void verify_completed_and_invalid_waits()
{
    using namespace galay::kernel;
    require(!detail::wait_task_completion(TaskRef{}), "invalid state must reject wait");

    auto task = complete_with_result();
    TaskRef reference = detail::TaskAccess::detach_task(std::move(task));
    require(reference.is_valid(), "allocate completed-before-wait task");
    reference.state()->m_handle.resume();
    JoinHandle<std::array<int, 16>> handle(std::move(reference));
    require(handle.wait().has_value(), "completion before wait must not block");
    require(handle.wait().has_value(), "repeated wait must not consume result");
    const auto result = handle.join();
    require(result.has_value() && result->front() == 42 && result->back() == 42,
            "wait must preserve heap-backed result");
    const auto repeated = handle.join();
    require(!repeated.has_value() &&
                repeated.error().code() == detail::TaskResultErrorCode::kAlreadyConsumed,
            "repeated join must report result already consumed");
}

void verify_shared_waiters()
{
    using namespace galay::kernel;
    constexpr int kWaiterCount = 8;
    for (int iteration = 0; iteration < 64; ++iteration) {
        auto task = complete_with_result();
        TaskRef reference = detail::TaskAccess::task_ref(task);
        require(reference.is_valid(), "allocate multi-waiter task");
        JoinHandle<std::array<int, 16>> handle(reference);
        std::atomic<int> started{0};
        int published_value = 0;
        std::vector<std::thread> waiters;
        for (int index = 0; index < kWaiterCount; ++index) {
            auto& waiter = waiters.emplace_back([owner = reference, &started, &published_value]() {
                const int previous = started.fetch_add(1, std::memory_order_release);
                require(previous < kWaiterCount, "each shared waiter must start once");
                require(detail::wait_task_completion(owner), "shared waiter must observe completion");
                require(published_value == 42, "completion must publish preceding non-atomic writes");
                require(detail::wait_task_completion(owner), "shared waiter must support repeated wait");
            });
            require(waiter.joinable(), "shared waiter thread must be joinable");
        }
        while (started.load(std::memory_order_acquire) != kWaiterCount) {
            std::this_thread::yield();
        }
        // Complete only after at least one waiter has registered. Registration
        // may precede its actual sleep, so this also exercises early notification.
        while (reference.state()->m_completion_status.load(std::memory_order_acquire) ==
               detail::TaskCompletionStatus::kPending) {
            std::this_thread::yield();
        }
        require(!task.done() && !reference.state()->is_done(),
                "waiting must not mark an unfinished task complete");
        published_value = 42;
        reference.state()->m_handle.resume();
        // The waiter and result consumer references keep the completed state alive.
        reference = TaskRef{};
        task = Task<std::array<int, 16>>{};
        for (auto& waiter : waiters) {
            waiter.join();
        }
        const auto result = handle.join();
        require(result.has_value() && result->front() == 42 && result->back() == 42,
                "all waiters must leave the result available for one join");
    }
}

} // namespace

int main()
{
    using namespace galay::kernel;
    verify_completed_and_invalid_waits();
    verify_shared_waiters();
    auto runtime = RuntimeBuilder().io_scheduler_count(2).parallel_scheduler_count(0).build();
    require(runtime.start().has_value(), "start completion race runtime");
    for (int iteration = 0; iteration < 50000; ++iteration) {
        std::vector<JoinHandle<void>> pending;
        for (std::size_t index = 0; index < runtime.get_io_scheduler_count(); ++index) {
            auto task = complete_immediately();
            const auto& reference = detail::TaskAccess::task_ref(task);
            auto* scheduler = runtime.get_io_scheduler(index);
            detail::set_task_runtime(reference, &runtime);
            detail::set_task_scheduler(reference, scheduler);
            require(scheduler && scheduler->schedule(reference), "submit owner completion task");
            const auto& submitted = pending.emplace_back(detail::TaskAccess::detach_task(std::move(task)));
            require(submitted.is_valid(), "retain completion state after submission");
        }
        for (auto& task : pending) {
            require(task.wait().has_value(), "wait must observe concurrent completion");
            require(task.join().has_value(), "join must observe concurrent completion");
        }
    }
    runtime.stop();
    require(std::puts("Concurrent task completion and blocking join passed") >= 0, "write result");
}
