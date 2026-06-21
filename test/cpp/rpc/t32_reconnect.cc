#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_reconnect.h>
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

class ReconnectService final : public RpcService {
public:
    ReconnectService()
        : RpcService("ReconnectService")
    {
        registerMethod("echo", &ReconnectService::echo);
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
    return static_cast<uint16_t>(35000 + (::getpid() % 7000));
}

template<typename AwaitResult>
bool callPayloadEquals(const AwaitResult& result, const std::string& expected)
{
    if (!result.has_value() || !result->has_value() || !result->value().has_value()) {
        return false;
    }
    const auto& payload = result->value()->payload();
    return result->value()->isOk() && std::string(payload.begin(), payload.end()) == expected;
}

Task<void> startServerLater(uint16_t port, std::shared_ptr<RpcServer>* server_slot)
{
    co_await sleep(std::chrono::milliseconds(60));
    auto server = std::make_shared<RpcServer>(RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .buildConfig());
    server->registerService(std::make_shared<ReconnectService>());
    server->start();
    *server_slot = std::move(server);
    co_return;
}

Task<void> runReconnectClient(uint16_t port, TestState* state)
{
    RpcReconnectPolicy policy;
    policy.max_attempts = 20;
    policy.backoff = std::chrono::milliseconds(10);

    RpcClient client;
    client.reconnectPolicy(policy);
    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result.has_value()) {
        state->ok = false;
        state->error = "connect retry did not succeed after delayed server start";
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto first = co_await client.call("ReconnectService", "echo", "first");
    if (!callPayloadEquals(first, "first")) {
        state->ok = false;
        state->error = "first call failed after reconnect connect";
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    co_await client.close();
    auto second = co_await client.call("ReconnectService", "echo", "second");
    if (!callPayloadEquals(second, "second")) {
        state->ok = false;
        state->error = "next call did not reconnect after local close";
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
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    std::shared_ptr<RpcServer> server;
    auto starter = runtime.spawn(startServerLater(port, &server));
    TestState state;
    auto client = runtime.spawn(runReconnectClient(port, &state));
    if (!starter.has_value() || !client.has_value()) {
        runtime.stop();
        std::cerr << "failed to schedule reconnect tasks\n";
        return 1;
    }

    for (int i = 0; i < 500 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();
    if (server) {
        server->stop();
    }

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "reconnect test timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }

    std::cout << "RPC reconnect PASS\n";
    return 0;
}
