#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

class HeartbeatService final : public RpcService {
public:
    explicit HeartbeatService(std::atomic<int>* calls)
        : RpcService("HeartbeatService")
        , m_calls(calls)
    {
        registerMethod("echo", &HeartbeatService::echo);
    }

    Task<void> echo(RpcContext& ctx)
    {
        m_calls->fetch_add(1, std::memory_order_relaxed);
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }

private:
    std::atomic<int>* m_calls;
};

struct TestState {
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
};

uint16_t loopbackPort()
{
    return static_cast<uint16_t>(33000 + (::getpid() % 9000));
}

template<typename AwaitResult>
bool okResult(const AwaitResult& result)
{
    if (!result.has_value()) {
        return false;
    }
    const auto& call_result = result.value();
    return call_result.has_value();
}

Task<void> runHeartbeatClient(uint16_t port, TestState* state, std::atomic<int>* route_calls)
{
    RpcClient client;
    bool connected = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto connect_result = co_await client.connect("127.0.0.1", port);
        if (connect_result.has_value()) {
            connected = true;
            break;
        }
        co_await sleep(std::chrono::milliseconds(10));
    }
    if (!connected) {
        state->ok = false;
        state->error = "client connect retry exhausted";
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto heartbeat = co_await client.sendHeartbeat();
    if (!okResult(heartbeat)) {
        state->ok = false;
        state->error = "heartbeat did not receive pong";
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (route_calls->load(std::memory_order_acquire) != 0) {
        state->ok = false;
        state->error = "heartbeat reached service route";
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto echo = co_await client.call("HeartbeatService", "echo", "ok");
    if (!okResult(echo) || route_calls->load(std::memory_order_acquire) != 1) {
        state->ok = false;
        state->error = "normal call failed after heartbeat";
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    co_await client.close();
    state->done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    const uint16_t port = loopbackPort();
    std::atomic<int> route_calls{0};

    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    HeartbeatService service(&route_calls);
    auto registered = server.registerService(service);
    if (!registered.has_value()) {
        std::cerr << "failed to register heartbeat service: "
                  << registered.error().message() << "\n";
        return 1;
    }
    auto server_started = server.start();
    if (!server_started.has_value()) {
        std::cerr << "failed to start heartbeat server: "
                  << server_started.error().message() << "\n";
        return 1;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(2).computeSchedulerCount(0).build();
    auto runtime_started = runtime.start();
    if (!runtime_started.has_value()) {
        server.stop();
        std::cerr << "failed to start heartbeat runtime: "
                  << runtime_started.error().message() << "\n";
        return 1;
    }

    TestState state;
    auto root = runtime.spawn(runHeartbeatClient(port, &state, &route_calls));
    if (!root.has_value()) {
        runtime.stop();
        server.stop();
        std::cerr << "failed to schedule heartbeat client\n";
        return 1;
    }

    for (int i = 0; i < 300 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();
    server.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "heartbeat test timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }

    std::cout << "RPC heartbeat PASS\n";
    return 0;
}
