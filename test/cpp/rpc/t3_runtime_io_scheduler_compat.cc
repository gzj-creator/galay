#include <galay/cpp/galay-rpc/utils/runtime_compat.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

Task<void> mark_done(std::atomic<bool>* done)
{
    done->store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    const size_t io_count = resolve_io_scheduler_count(0);
    if (io_count == 0) {
        return 1;
    }

    Runtime runtime = RuntimeBuilder().io_scheduler_count(io_count).parallel_scheduler_count(1).build();
    runtime.start();

    auto* scheduler = runtime.get_next_io_scheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        return 2;
    }

    std::atomic<bool> done{false};
    if (!schedule_task(scheduler, mark_done(&done))) {
        runtime.stop();
        return 3;
    }

    for (int i = 0; i < 20 && !done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    runtime.stop();
    return done.load(std::memory_order_acquire) ? 0 : 4;
}
