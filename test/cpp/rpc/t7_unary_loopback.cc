#include "result_writer.h"

#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

using namespace galay::kernel;
using namespace galay::rpc;
using namespace std::chrono_literals;

namespace {

uint16_t testPort()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return static_cast<uint16_t>(24000 + (static_cast<uint64_t>(now) % 20000));
}

std::string payloadString(const RpcResponse& response)
{
    return std::string(response.payload().data(), response.payload().size());
}

class LoopbackService : public RpcService {
public:
    LoopbackService() : RpcService("LoopbackService")
    {
        registerMethod("echo", &LoopbackService::echo);
        registerMethod("reverse", &LoopbackService::reverse);
        registerMethod("length", &LoopbackService::length);
        registerClientStreamingMethod("echo", &LoopbackService::echo);
        registerServerStreamingMethod("echo", &LoopbackService::echo);
        registerBidiStreamingMethod("echo", &LoopbackService::echo);
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }

    Task<void> reverse(RpcContext& ctx)
    {
        std::string payload(ctx.request().payload().begin(), ctx.request().payload().end());
        std::reverse(payload.begin(), payload.end());
        ctx.setPayload(payload);
        co_return;
    }

    Task<void> length(RpcContext& ctx)
    {
        const std::string length = std::to_string(ctx.request().payloadSize());
        ctx.setPayload(length);
        co_return;
    }
};

struct ClientResult {
    bool connected = false;
    bool ok = false;
    std::string failure;
};

void fail(ClientResult* result, std::string message)
{
    result->ok = false;
    if (result->failure.empty()) {
        result->failure = std::move(message);
    }
}

bool expectResponse(ClientResult* result,
                    const RpcCallResult& call,
                    RpcErrorCode code,
                    std::string_view payload,
                    RpcCallMode mode)
{
    if (!call.has_value() || !call->has_value()) {
        fail(result, "call returned transport error or empty response");
        return false;
    }

    const RpcResponse& response = call->value();
    if (response.errorCode() != code ||
        response.callMode() != mode ||
        payloadString(response) != payload) {
        fail(result,
             "unexpected response code=" + std::to_string(static_cast<int>(response.errorCode())) +
             " mode=" + std::to_string(static_cast<int>(response.callMode())) +
             " payload=" + payloadString(response));
        return false;
    }
    return true;
}

template <typename TaskResult>
bool expectTaskResponse(ClientResult* result,
                        const TaskResult& task_result,
                        RpcErrorCode code,
                        std::string_view payload,
                        RpcCallMode mode)
{
    if (!task_result.has_value()) {
        fail(result, "task error: " + std::string(task_result.error().message()));
        return false;
    }
    return expectResponse(result, task_result.value(), code, payload, mode);
}

Task<void> runClient(uint16_t port, ClientResult* result)
{
    RpcClient client = RpcClientBuilder().ringBufferSize(64 * 1024).build();
    auto connected = co_await client.connect("127.0.0.1", port).timeout(200ms);
    if (!connected) {
        result->connected = false;
        fail(result, "connect failed: " + connected.error().message());
        co_return;
    }
    result->connected = true;

    auto echo = co_await client.call("LoopbackService", "echo", "hello");
    if (!expectTaskResponse(result, echo, RpcErrorCode::OK, "hello", RpcCallMode::UNARY)) {
        (void)co_await client.close();
        co_return;
    }

    auto reverse = co_await client.call("LoopbackService", "reverse", "desserts");
    if (!expectTaskResponse(result, reverse, RpcErrorCode::OK, "stressed", RpcCallMode::UNARY)) {
        (void)co_await client.close();
        co_return;
    }

    auto length = co_await client.call("LoopbackService", "length", "1234567");
    if (!expectTaskResponse(result, length, RpcErrorCode::OK, "7", RpcCallMode::UNARY)) {
        (void)co_await client.close();
        co_return;
    }

    auto missing_service = co_await client.call("MissingService", "echo", "x");
    if (!expectTaskResponse(result, missing_service, RpcErrorCode::SERVICE_NOT_FOUND, "", RpcCallMode::UNARY)) {
        (void)co_await client.close();
        co_return;
    }

    auto missing_method = co_await client.call("LoopbackService", "missing", "x");
    if (!expectTaskResponse(result, missing_method, RpcErrorCode::METHOD_NOT_FOUND, "", RpcCallMode::UNARY)) {
        (void)co_await client.close();
        co_return;
    }

    auto client_stream = co_await client.callClientStreamFrame(
        "LoopbackService", "echo", "client", 6, true);
    if (!expectTaskResponse(result, client_stream, RpcErrorCode::OK, "client", RpcCallMode::CLIENT_STREAMING)) {
        (void)co_await client.close();
        co_return;
    }

    auto server_stream = co_await client.callServerStreamRequest(
        "LoopbackService", "echo", "server", 6);
    if (!expectTaskResponse(result, server_stream, RpcErrorCode::OK, "server", RpcCallMode::SERVER_STREAMING)) {
        (void)co_await client.close();
        co_return;
    }

    auto bidi_stream = co_await client.callBidiStreamFrame(
        "LoopbackService", "echo", "bidi", 4, true);
    if (!expectTaskResponse(result, bidi_stream, RpcErrorCode::OK, "bidi", RpcCallMode::BIDI_STREAMING)) {
        (void)co_await client.close();
        co_return;
    }

    result->ok = true;
    result->failure.clear();
    (void)co_await client.close();
}

bool runClientWithRetry(uint16_t port, ClientResult* result)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        ClientResult attempt_result;
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        auto run_result = runtime.blockOn(runClient(port, &attempt_result));
        runtime.stop();

        if (!run_result.has_value()) {
            attempt_result.failure = std::string(run_result.error().message());
        }

        if (attempt_result.ok) {
            *result = std::move(attempt_result);
            return true;
        }

        if (attempt_result.connected) {
            *result = std::move(attempt_result);
            return false;
        }

        *result = std::move(attempt_result);
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

} // namespace

int main()
{
    test::TestResultWriter writer("t7_unary_loopback.result");
    std::cout << "Running RPC unary loopback tests...\n";

    const uint16_t port = testPort();
    auto service = std::make_shared<LoopbackService>();
    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .ringBufferSize(64 * 1024)
        .build();
    server.registerService(service);
    server.start();

    ClientResult result;
    const bool passed = runClientWithRetry(port, &result);

    server.stop();

    writer.writeTestCase("RpcServer/RpcClient unary loopback and mode routes",
        passed && result.ok,
        result.failure);
    writer.writeSummary();

    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";
    return writer.failed() > 0 ? 1 : 0;
}
