#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_interceptor.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

class AuthService final : public RpcService {
public:
    AuthService()
        : RpcService("AuthService")
    {
        registerMethod("echo", &AuthService::echo);
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }
};

uint16_t loopbackPort()
{
    return static_cast<uint16_t>(43000 + (::getpid() % 3000));
}

struct State {
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
};

bool payloadEquals(const RpcCallResult& result, const std::string& expected)
{
    if (!result.has_value() || !result->has_value()) {
        return false;
    }
    const auto& payload = result->value().payload();
    return result->value().isOk() && std::string(payload.begin(), payload.end()) == expected;
}

Task<void> runClient(uint16_t port, State* state)
{
    RpcCallOptions options;
    if (!options.metadata().insert("authorization", "token").has_value()) {
        state->ok = false;
        state->error = "call options metadata rejected valid authorization key";
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    RpcClient client;
    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result.has_value() || !connect_result->has_value()) {
        state->ok = false;
        state->error = "client connect failed";
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    RpcCallOptions missing_options;
    auto rejected_task = co_await client.call("AuthService", "echo", "x", missing_options);
    if (!rejected_task.has_value() || !rejected_task->has_value() ||
        !rejected_task->value().has_value() ||
        rejected_task->value()->errorCode() != RpcErrorCode::UNAUTHENTICATED) {
        state->ok = false;
        state->error = "auth interceptor did not reject with UNAUTHENTICATED";
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto allowed_task = co_await client.call("AuthService", "echo", "ok", options);
    if (!allowed_task.has_value() || !payloadEquals(*allowed_task, "ok")) {
        state->ok = false;
        state->error = "auth interceptor blocked allowed service";
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
    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .interceptor([](const RpcRequest& request) -> std::expected<void, RpcError> {
            auto authorization = request.metadata().get("authorization");
            if (!authorization.has_value() || *authorization != "token") {
                return std::unexpected(RpcError(RpcErrorCode::UNAUTHENTICATED, "missing token"));
            }
            return {};
        })
        .build();
    AuthService service;
    auto registered = server.registerService(service);
    if (!registered.has_value()) {
        std::cerr << "failed to register auth service: "
                  << registered.error().message() << "\n";
        return 1;
    }
    auto server_started = server.start();
    if (!server_started.has_value()) {
        std::cerr << "failed to start auth server: "
                  << server_started.error().message() << "\n";
        return 1;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto runtime_started = runtime.start();
    if (!runtime_started.has_value()) {
        server.stop();
        std::cerr << "failed to start auth runtime: "
                  << runtime_started.error().message() << "\n";
        return 1;
    }
    State state;
    auto scheduled = runtime.spawn(runClient(port, &state));
    if (!scheduled.has_value()) {
        runtime.stop();
        server.stop();
        std::cerr << "failed to schedule auth client\n";
        return 1;
    }

    for (int i = 0; i < 300 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    runtime.stop();
    server.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "auth interceptor test timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }
    std::cout << "RPC auth interceptor PASS\n";
    return 0;
}
