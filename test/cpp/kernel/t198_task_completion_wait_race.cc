#include <galay/cpp/galay-kernel/core/runtime.h>

#include <cstdio>
#include <cstdlib>
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

} // namespace

int main()
{
    using namespace galay::kernel;
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
