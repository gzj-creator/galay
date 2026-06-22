#include <galay/cpp/galay-rpc/kernel/rpc_managed_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
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

class ManagedService final : public RpcService {
public:
    ManagedService()
        : RpcService("ManagedService")
    {
        registerMethod("echo", &ManagedService::echo);
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }
};

struct TestState {
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
};

uint16_t loopbackPort()
{
    return static_cast<uint16_t>(42000 + (::getpid() % 5000));
}

void fail(TestState* state, std::string message)
{
    state->ok = false;
    state->error = std::move(message);
}

template<typename AwaitResult>
bool payloadEquals(const AwaitResult& result, const std::string& expected)
{
    if (!result.has_value() || !result->has_value() || !result->value().has_value()) {
        return false;
    }
    const auto& payload = result->value()->payload();
    return result->value()->isOk() && std::string(payload.begin(), payload.end()) == expected;
}

Task<void> runManagedChecks(uint16_t port, TestState* state)
{
    RpcStaticDiscovery discovery;
    discovery.set("ManagedService", {
        RpcEndpoint{"127.0.0.1", static_cast<uint16_t>(port + 1)},
        RpcEndpoint{"127.0.0.1", port},
    });

    RpcManagedClientConfig config;
    config.pool.max_connections_per_endpoint = 1;
    config.pool.max_waiters_per_endpoint = 2;

    RpcManagedClient client(discovery, config);
    auto endpoints = client.refresh("ManagedService");
    if (!endpoints.has_value() || endpoints->size() != 2) {
        fail(state, "fake discovery did not return two endpoints");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto selected_a = client.selectEndpoint("ManagedService");
    auto selected_b = client.selectEndpoint("ManagedService");
    if (!selected_a.has_value() || !selected_b.has_value() ||
        selected_a->port != static_cast<uint16_t>(port + 1) || selected_b->port != port) {
        fail(state, "round-robin endpoint selection failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    client.markEndpointUnavailable(*selected_a);
    auto selected_after_failure = client.selectEndpoint("ManagedService");
    if (!selected_after_failure.has_value() || selected_after_failure->port != port) {
        fail(state, "endpoint failure did not select next allowed endpoint");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    client.markEndpointUnavailable(*selected_after_failure);
    auto recovered_after_all_unavailable = client.selectEndpoint("ManagedService");
    if (!recovered_after_all_unavailable.has_value()) {
        fail(state, "all-unavailable endpoints were not reopened for transient recovery");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    bool connected = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto result = co_await client.call("ManagedService", "echo", "managed");
        if (payloadEquals(result, "managed")) {
            connected = true;
            break;
        }
        co_await sleep(std::chrono::milliseconds(10));
    }
    if (!connected) {
        fail(state, "real loopback managed call failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    RpcClient direct;
    auto connect_result = co_await direct.connect("127.0.0.1", port);
    if (!connect_result.has_value()) {
        fail(state, "existing RpcClient direct connect failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (!connect_result->has_value()) {
        fail(state, "existing RpcClient direct connect returned IO error");
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    auto direct_result = co_await direct.call("ManagedService", "echo", "direct");
    co_await direct.close();
    if (!payloadEquals(direct_result, "direct")) {
        fail(state, "existing RpcClient direct call failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto shutdown = client.shutdown();
    if (!shutdown.has_value()) {
        fail(state, "managed client shutdown failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    state->done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    const uint16_t port = loopbackPort();

    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    server.registerService(std::make_shared<ManagedService>());
    server.start();

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    TestState state;
    auto scheduled = runtime.spawn(runManagedChecks(port, &state));
    if (!scheduled.has_value()) {
        runtime.stop();
        server.stop();
        std::cerr << "failed to schedule managed client checks\n";
        return 1;
    }

    for (int i = 0; i < 500 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();
    server.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "managed client test timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }

    std::cout << "RPC managed client PASS\n";
    return 0;
}
