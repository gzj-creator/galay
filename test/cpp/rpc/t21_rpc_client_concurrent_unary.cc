#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
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
#include <vector>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

class ConcurrentUnaryService final : public RpcService {
public:
    ConcurrentUnaryService()
        : RpcService("ConcurrentUnaryService")
    {
        registerMethod("echo", &ConcurrentUnaryService::echo);
    }

    Task<void> echo(RpcContext& ctx)
    {
        const auto& payload = ctx.request().payload();
        if (!payload.empty()) {
            const unsigned char tail = static_cast<unsigned char>(payload.back());
            co_await sleep(std::chrono::milliseconds(tail % 5));
        }
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }
};

struct SharedState {
    std::atomic<int> completed{0};
    std::atomic<int> failed{0};
};

uint16_t loopbackPort()
{
    return static_cast<uint16_t>(26000 + (::getpid() % 16000));
}

bool payloadEquals(const RpcResponse& response, const std::string& expected)
{
    const auto& payload = response.payload();
    return std::string(payload.begin(), payload.end()) == expected;
}

template<typename AwaitResult>
const RpcResponse* responsePtr(const AwaitResult& result)
{
    if (!result.has_value()) {
        return nullptr;
    }
    const auto& call_result = result.value();
    if (!call_result.has_value() || !call_result->has_value()) {
        return nullptr;
    }
    return &call_result->value();
}

Task<void> runOneCall(RpcClient* client, int index, SharedState* state)
{
    const std::string payload = "payload-" + std::to_string(index);
    auto result = co_await client->call("ConcurrentUnaryService", "echo", payload);
    const RpcResponse* response = responsePtr(result);
    if (response == nullptr || !response->isOk() || !payloadEquals(*response, payload)) {
        state->failed.fetch_add(1, std::memory_order_relaxed);
    }
    state->completed.fetch_add(1, std::memory_order_release);
    co_return;
}

Task<void> runConcurrentClient(uint16_t port, SharedState* state)
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
        state->failed.fetch_add(64, std::memory_order_relaxed);
        state->completed.fetch_add(64, std::memory_order_release);
        co_return;
    }

    auto runtime = RuntimeHandle::current();
    if (!runtime.has_value()) {
        state->failed.fetch_add(64, std::memory_order_relaxed);
        state->completed.fetch_add(64, std::memory_order_release);
        co_await client.close();
        co_return;
    }

    for (int i = 0; i < 64; ++i) {
        auto spawned = runtime->spawn(runOneCall(&client, i, state));
        if (!spawned.has_value()) {
            state->failed.fetch_add(1, std::memory_order_relaxed);
            state->completed.fetch_add(1, std::memory_order_release);
        }
    }

    while (state->completed.load(std::memory_order_acquire) < 64) {
        co_await sleep(std::chrono::milliseconds(1));
    }

    co_await client.close();
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
    server.registerService(std::make_shared<ConcurrentUnaryService>());
    server.start();

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(2).computeSchedulerCount(0).build();
    runtime.start();

    SharedState state;
    auto root = runtime.spawn(runConcurrentClient(port, &state));
    if (!root.has_value()) {
        runtime.stop();
        server.stop();
        std::cerr << "failed to schedule concurrent client\n";
        return 1;
    }

    for (int i = 0; i < 500 && state.completed.load(std::memory_order_acquire) < 64; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();
    server.stop();

    const int completed = state.completed.load(std::memory_order_acquire);
    const int failed = state.failed.load(std::memory_order_acquire);
    if (completed != 64) {
        std::cerr << "concurrent unary timed out: completed=" << completed << " failed=" << failed << "\n";
        return 1;
    }
    if (failed != 0) {
        std::cerr << "concurrent unary failed: failed=" << failed << "\n";
        return 1;
    }

    std::cout << "RPC client concurrent unary PASS\n";
    return 0;
}
