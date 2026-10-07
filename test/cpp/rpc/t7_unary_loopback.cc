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
        register_method("echo", &LoopbackService::echo);
        register_client_streaming_method("echo", &LoopbackService::echo);
        register_server_streaming_method("echo", &LoopbackService::echo);
        register_bidi_streaming_method("echo", &LoopbackService::echo);
        register_method("reverse", &LoopbackService::reverse);
        register_method("length", &LoopbackService::length);
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.set_payload(ctx.request().payload_view());
        co_return;
    }

    Task<void> reverse(RpcContext& ctx)
    {
        auto payload = ctx.request().payload();
        std::reverse(payload.begin(), payload.end());
        ctx.set_payload(payload.data(), payload.size());
        co_return;
    }

    Task<void> length(RpcContext& ctx)
    {
        const uint32_t len = static_cast<uint32_t>(ctx.request().payload_size());
        ctx.set_payload(reinterpret_cast<const char*>(&len), sizeof(len));
        co_return;
    }
};

struct CallResult {
    bool done = false;
    bool ok = true;
    std::string error;
};

uint16_t loopback_port()
{
    return static_cast<uint16_t>(22000 + (::getpid() % 20000));
}

void fail(CallResult& state, std::string message)
{
    state.ok = false;
    state.error = std::move(message);
}

bool payload_equals(const RpcResponse& response, const std::string& expected)
{
    const auto& payload = response.payload();
    return std::string(payload.begin(), payload.end()) == expected;
}

template<typename AwaitResult>
const RpcResponse* response_ptr(const AwaitResult& result)
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

Task<void> run_unary_client(uint16_t port, CallResult* state)
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
    const RpcResponse* echo_response = response_ptr(echo_result);
    if (echo_response == nullptr || !echo_response->is_ok() ||
        !payload_equals(*echo_response, "hello")) {
        fail(*state, "echo call failed");
        co_await client.close();
        state->done = true;
        co_return;
    }

    auto reverse_result = co_await client.call("LoopbackService", "reverse", "abcdef");
    const RpcResponse* reverse_response = response_ptr(reverse_result);
    if (reverse_response == nullptr || !reverse_response->is_ok() ||
        !payload_equals(*reverse_response, "fedcba")) {
        fail(*state, "reverse call failed");
        co_await client.close();
        state->done = true;
        co_return;
    }

    auto length_result = co_await client.call("LoopbackService", "length", "abcd");
    uint32_t length_value = 0;
    const RpcResponse* length_response = response_ptr(length_result);
    if (length_response == nullptr || !length_response->is_ok() ||
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
    const RpcResponse* missing_service_response = response_ptr(missing_service);
    if (missing_service_response == nullptr ||
        missing_service_response->error_code() != RpcErrorCode::SERVICE_NOT_FOUND) {
        fail(*state, "missing service did not return SERVICE_NOT_FOUND");
        co_await client.close();
        state->done = true;
        co_return;
    }

    auto missing_method = co_await client.call("LoopbackService", "missing", "x");
    const RpcResponse* missing_method_response = response_ptr(missing_method);
    if (missing_method_response == nullptr ||
        missing_method_response->error_code() != RpcErrorCode::METHOD_NOT_FOUND) {
        fail(*state, "missing method did not return METHOD_NOT_FOUND");
        co_await client.close();
        state->done = true;
        co_return;
    }

    const std::string mode_payload = "mode-payload";
    auto client_stream = co_await client.call_client_stream_frame(
        "LoopbackService", "echo", mode_payload.data(), mode_payload.size(), true);
    auto server_stream = co_await client.call_server_stream_request(
        "LoopbackService", "echo", mode_payload.data(), mode_payload.size());
    auto bidi_stream = co_await client.call_bidi_stream_frame(
        "LoopbackService", "echo", mode_payload.data(), mode_payload.size(), true);

    const RpcResponse* client_stream_response = response_ptr(client_stream);
    const RpcResponse* server_stream_response = response_ptr(server_stream);
    const RpcResponse* bidi_stream_response = response_ptr(bidi_stream);
    if (client_stream_response == nullptr ||
        client_stream_response->call_mode() != RpcCallMode::CLIENT_STREAMING ||
        !payload_equals(*client_stream_response, mode_payload) ||
        server_stream_response == nullptr ||
        server_stream_response->call_mode() != RpcCallMode::SERVER_STREAMING ||
        !payload_equals(*server_stream_response, mode_payload) ||
        bidi_stream_response == nullptr ||
        bidi_stream_response->call_mode() != RpcCallMode::BIDI_STREAMING ||
        !payload_equals(*bidi_stream_response, mode_payload)) {
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
    const uint16_t port = loopback_port();

    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .build();
    LoopbackService service;
    auto registered = server.register_service(service);
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

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    auto runtime_started = runtime.start();
    if (!runtime_started.has_value()) {
        server.stop();
        std::cerr << "failed to start loopback runtime: "
                  << runtime_started.error().message() << "\n";
        return 1;
    }

    CallResult state;
    if (!schedule_task(runtime.get_next_io_scheduler(), run_unary_client(port, &state))) {
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
