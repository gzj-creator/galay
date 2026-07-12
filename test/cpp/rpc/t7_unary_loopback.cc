/**
 * @file t7_unary_loopback.cc
 * @brief RPC unary loopback 测试
 */

#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

class LoopbackService final : public RpcService {
public:
    LoopbackService()
        : RpcService("LoopbackService")
    {
        registerMethod("echo", &LoopbackService::echo);
        registerClientStreamingMethod("echo", &LoopbackService::echo);
        registerServerStreamingMethod("echo", &LoopbackService::echo);
        registerBidiStreamingMethod("echo", &LoopbackService::echo);
        registerMethod("reverse", &LoopbackService::reverse);
        registerMethod("length", &LoopbackService::length);
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }

    Task<void> reverse(RpcContext& ctx)
    {
        auto payload = ctx.request().payload();
        std::reverse(payload.begin(), payload.end());
        ctx.setPayload(payload.data(), payload.size());
        co_return;
    }

    Task<void> length(RpcContext& ctx)
    {
        const uint32_t len = static_cast<uint32_t>(ctx.request().payloadSize());
        ctx.setPayload(reinterpret_cast<const char*>(&len), sizeof(len));
        co_return;
    }
};

struct CallResult {
    bool done = false;
    bool ok = true;
    std::string error;
};

uint16_t loopbackPort()
{
    return static_cast<uint16_t>(22000 + (::getpid() % 20000));
}

void fail(CallResult& state, std::string message)
{
    state.ok = false;
    state.error = std::move(message);
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

Task<void> runUnaryClient(uint16_t port, CallResult* state)
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
        fail(*state, "client connect retry exhausted");
        state->done = true;
        co_return;
    }

    auto echo_result = co_await client.call("LoopbackService", "echo", "hello");
    const RpcResponse* echo_response = responsePtr(echo_result);
    if (echo_response == nullptr || !echo_response->isOk() ||
        !payloadEquals(*echo_response, "hello")) {
        fail(*state, "echo call failed");
        co_await client.close();
        state->done = true;
        co_return;
    }

    auto reverse_result = co_await client.call("LoopbackService", "reverse", "abcdef");
    const RpcResponse* reverse_response = responsePtr(reverse_result);
    if (reverse_response == nullptr || !reverse_response->isOk() ||
        !payloadEquals(*reverse_response, "fedcba")) {
        fail(*state, "reverse call failed");
        co_await client.close();
        state->done = true;
        co_return;
    }

    auto length_result = co_await client.call("LoopbackService", "length", "abcd");
    uint32_t length_value = 0;
    const RpcResponse* length_response = responsePtr(length_result);
    if (length_response == nullptr || !length_response->isOk() ||
        length_response->payload().size() != sizeof(length_value)) {
        fail(*state, "length call failed");
        co_await client.close();
        state->done = true;
        co_return;
    }
    std::memcpy(&length_value, length_response->payload().data(), sizeof(length_value));
    if (length_value != 4) {
        fail(*state, "length payload mismatch");
        co_await client.close();
        state->done = true;
        co_return;
    }

    auto missing_service = co_await client.call("MissingService", "echo", "x");
    const RpcResponse* missing_service_response = responsePtr(missing_service);
    if (missing_service_response == nullptr ||
        missing_service_response->errorCode() != RpcErrorCode::SERVICE_NOT_FOUND) {
        fail(*state, "missing service did not return SERVICE_NOT_FOUND");
        co_await client.close();
        state->done = true;
        co_return;
    }

    auto missing_method = co_await client.call("LoopbackService", "missing", "x");
    const RpcResponse* missing_method_response = responsePtr(missing_method);
    if (missing_method_response == nullptr ||
        missing_method_response->errorCode() != RpcErrorCode::METHOD_NOT_FOUND) {
        fail(*state, "missing method did not return METHOD_NOT_FOUND");
        co_await client.close();
        state->done = true;
        co_return;
    }

    const std::string mode_payload = "mode-payload";
    auto client_stream = co_await client.callClientStreamFrame(
        "LoopbackService", "echo", mode_payload.data(), mode_payload.size(), true);
    auto server_stream = co_await client.callServerStreamRequest(
        "LoopbackService", "echo", mode_payload.data(), mode_payload.size());
    auto bidi_stream = co_await client.callBidiStreamFrame(
        "LoopbackService", "echo", mode_payload.data(), mode_payload.size(), true);

    const RpcResponse* client_stream_response = responsePtr(client_stream);
    const RpcResponse* server_stream_response = responsePtr(server_stream);
    const RpcResponse* bidi_stream_response = responsePtr(bidi_stream);
    if (client_stream_response == nullptr ||
        client_stream_response->callMode() != RpcCallMode::CLIENT_STREAMING ||
        !payloadEquals(*client_stream_response, mode_payload) ||
        server_stream_response == nullptr ||
        server_stream_response->callMode() != RpcCallMode::SERVER_STREAMING ||
        !payloadEquals(*server_stream_response, mode_payload) ||
        bidi_stream_response == nullptr ||
        bidi_stream_response->callMode() != RpcCallMode::BIDI_STREAMING ||
        !payloadEquals(*bidi_stream_response, mode_payload)) {
        fail(*state, "stream-mode compatibility call failed");
        co_await client.close();
        state->done = true;
        co_return;
    }

    co_await client.close();
    state->done = true;
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
    LoopbackService service;
    auto registered = server.registerService(service);
    if (!registered.has_value()) {
        std::cerr << "failed to register loopback service: "
                  << registered.error().message() << "\n";
        return 1;
    }
    auto server_started = server.start();
    if (!server_started.has_value()) {
        std::cerr << "failed to start loopback server: "
                  << server_started.error().message() << "\n";
        return 1;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto runtime_started = runtime.start();
    if (!runtime_started.has_value()) {
        server.stop();
        std::cerr << "failed to start loopback runtime: "
                  << runtime_started.error().message() << "\n";
        return 1;
    }

    CallResult state;
    if (!scheduleTask(runtime.getNextIOScheduler(), runUnaryClient(port, &state))) {
        runtime.stop();
        server.stop();
        std::cerr << "failed to schedule unary client\n";
        return 1;
    }

    for (int i = 0; i < 200 && !state.done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    runtime.stop();
    server.stop();

    if (!state.done) {
        std::cerr << "unary loopback timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }

    std::cout << "RPC unary loopback PASS\n";
    return 0;
}
