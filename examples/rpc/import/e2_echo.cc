/**
 * @file e2_echo.cc
 * @brief Echo RPC客户端示例（C++23 import 版本）
 */

import galay.rpc;

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

using namespace galay::rpc;
using namespace galay::kernel;

namespace {

const char* callModeToString(RpcCallMode mode) {
    switch (mode) {
        case RpcCallMode::UNARY:
            return "unary";
        case RpcCallMode::CLIENT_STREAMING:
            return "client_stream";
        case RpcCallMode::SERVER_STREAMING:
            return "server_stream";
        case RpcCallMode::BIDI_STREAMING:
            return "bidi";
        default:
            return "unknown";
    }
}

template<typename CallFn>
Coroutine callEchoWithMode(std::string_view title,
                           RpcCallMode expected_mode,
                           const std::string& payload,
                           CallFn&& call_fn) {
    std::cout << "=== " << title << " ===\n";
    std::cout << "Input: " << payload << "\n";

    auto result = co_await call_fn();
    if (!result) {
        std::cerr << "Transport error: " << result.error().message() << "\n\n";
        co_return;
    }

    if (!result.value().has_value()) {
        std::cerr << "No response received\n\n";
        co_return;
    }

    const auto& response = result.value().value().value();
    if (!response.isOk()) {
        std::cerr << "RPC error: " << rpcErrorCodeToString(response.errorCode()) << "\n\n";
        co_return;
    }

    std::string output(response.payload().begin(), response.payload().end());
    std::cout << "Output: " << output << "\n";
    std::cout << "Response mode: " << callModeToString(response.callMode())
              << ", end_of_stream=" << (response.endOfStream() ? "true" : "false") << "\n";

    if (response.callMode() != expected_mode) {
        std::cerr << "Mode mismatch: expected=" << callModeToString(expected_mode)
                  << ", actual=" << callModeToString(response.callMode()) << "\n";
    }

    std::cout << "\n";
    co_return;
}

} // namespace

Coroutine runClient(Runtime& runtime, const std::string& host, uint16_t port) {
    (void)runtime;
    std::cout << "Connecting to " << host << ":" << port << "...\n";

    RpcClient client;

    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "Failed to connect: " << connect_result.error().message() << "\n";
        co_return;
    }

    std::cout << "Connected!\n\n";

    const std::string payload = "Hello, 4-mode RPC World!";

    co_await callEchoWithMode("Unary Echo",
                              RpcCallMode::UNARY,
                              payload,
                              [&]() {
                                  return client.call("EchoService", "echo", payload);
                              });

    co_await callEchoWithMode("Client Streaming Echo (single frame)",
                              RpcCallMode::CLIENT_STREAMING,
                              payload,
                              [&]() {
                                  return client.callClientStreamFrame("EchoService",
                                                                      "echo",
                                                                      payload.data(),
                                                                      payload.size(),
                                                                      true);
                              });

    co_await callEchoWithMode("Server Streaming Echo (single response frame)",
                              RpcCallMode::SERVER_STREAMING,
                              payload,
                              [&]() {
                                  return client.callServerStreamRequest("EchoService",
                                                                        "echo",
                                                                        payload.data(),
                                                                        payload.size());
                              });

    co_await callEchoWithMode("Bidi Streaming Echo (single frame)",
                              RpcCallMode::BIDI_STREAMING,
                              payload,
                              [&]() {
                                  return client.callBidiStreamFrame("EchoService",
                                                                    "echo",
                                                                    payload.data(),
                                                                    payload.size(),
                                                                    true);
                              });

    co_await client.close();
    std::cout << "Client closed.\n";
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = static_cast<uint16_t>(std::atoi(argv[2]));
    }

    std::cout << "=== Echo RPC Client Example (4 Modes, import) ===\n\n";

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    (void)scheduleTask(scheduler, runClient(runtime, host, port));

    std::this_thread::sleep_for(std::chrono::seconds(4));

    runtime.stop();

    return 0;
}
