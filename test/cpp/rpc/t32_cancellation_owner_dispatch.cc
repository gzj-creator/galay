#include <galay/cpp/galay-rpc/kernel/rpc_channel.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <chrono>
#include <future>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace galay::rpc;
using namespace std::chrono_literals;

namespace {

struct OwnerAddress {
    Scheduler* scheduler;
    RpcCancellationSource* source;
};

Task<void> cancelOnOwner(RpcCancellationSource* source)
{
    source->cancel();
    co_return;
}

Task<void> awaitCancellation(std::promise<OwnerAddress>* address,
                             std::promise<bool>* done)
{
    RpcCancellationSource source;
    AsyncWaiter<int> waiter;
    const auto owner_thread = std::this_thread::get_id();
    bool owner_matched = false;
    bool notified = false;
    auto registration = source.token().registerCallback([&] {
        owner_matched = std::this_thread::get_id() == owner_thread;
        notified = waiter.notify(42);
    });
    auto* owner = co_await galay::rpc::detail::CurrentSchedulerAwaitable{};
    address->set_value({owner, &source});
    auto result = co_await waiter.wait();
    done->set_value(result.has_value() && *result == 42 &&
                    source.token().cancelled() && owner_matched && notified);
    co_return;
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(2).parallelSchedulerCount(0).build();
    auto started = runtime.start();
    if (!started.has_value()) return 1;
    std::promise<OwnerAddress> address;
    std::promise<bool> done;
    auto address_ready = address.get_future();
    auto completed = done.get_future();
    auto root = runtime.spawnIO(awaitCancellation(&address, &done));
    if (!root.has_value() || address_ready.wait_for(5s) != std::future_status::ready) {
        runtime.stop();
        return 2;
    }
    auto owner = address_ready.get();
    // This external thread transports a request; it never touches cancellation state.
    const bool submitted = scheduleTask(owner.scheduler, cancelOnOwner(owner.source));
    const bool ready = submitted && completed.wait_for(5s) == std::future_status::ready;
    bool passed = ready && completed.get();
    if (ready) {
        // Join is only used on the external driver thread, after completion notification.
        const auto joined = root->join();
        passed = passed && joined.has_value();
    }
    runtime.stop();
    if (!passed) return 3;
    std::cout << "RPC cancellation owner dispatch PASS\n";
    return 0;
}
