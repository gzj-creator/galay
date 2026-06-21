#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_metrics.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

class MetricsService final : public RpcService {
public:
    MetricsService() : RpcService("MetricsService") { registerMethod("echo", &MetricsService::echo); }
    Task<void> echo(RpcContext& ctx) { ctx.setPayload(ctx.request().payloadView()); co_return; }
};

struct State {
    std::atomic<bool> done{false};
    std::atomic<size_t> events{0};
    std::atomic<size_t> ok_events{0};
    bool ok = true;
    std::string error;
};

uint16_t port() { return static_cast<uint16_t>(44000 + (::getpid() % 2000)); }

Task<void> run(uint16_t p, State* state)
{
    RpcClientConfig config;
    config.channel_options.metrics_callback = [state](const RpcMetricEvent& event) {
        state->events.fetch_add(1, std::memory_order_relaxed);
        if (event.service == "MetricsService" && event.method == "echo" &&
            event.status == RpcErrorCode::OK && event.pending_calls == 0) {
            state->ok_events.fetch_add(1, std::memory_order_relaxed);
        }
    };
    RpcClient client(config);
    auto connect = co_await client.connect("127.0.0.1", p);
    if (!connect.has_value() || !connect->has_value()) {
        state->ok = false;
        state->error = "connect failed";
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    auto call = co_await client.call("MetricsService", "echo", "x");
    co_await client.close();
    if (!call.has_value() || !call->has_value() || state->ok_events.load() != 1) {
        state->ok = false;
        state->error = "metrics callback did not observe success event";
    }
    RpcPendingGauge gauge;
    gauge.increment();
    gauge.decrement();
    if (gauge.value() != 0) {
        state->ok = false;
        state->error = "pending gauge did not return to zero";
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    const auto p = port();
    auto server = RpcServerBuilder().host("127.0.0.1").port(p).ioSchedulerCount(1).computeSchedulerCount(0).build();
    server.registerService(std::make_shared<MetricsService>());
    server.start();

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();
    State state;
    auto scheduled = runtime.spawn(run(p, &state));
    if (!scheduled.has_value()) {
        runtime.stop();
        server.stop();
        std::cerr << "failed to schedule metrics test\n";
        return 1;
    }
    for (int i = 0; i < 300 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    runtime.stop();
    server.stop();
    if (!state.done.load(std::memory_order_acquire) || !state.ok) {
        std::cerr << (state.error.empty() ? "metrics test timed out" : state.error) << "\n";
        return 1;
    }
    std::cout << "RPC metrics PASS\n";
    return 0;
}
