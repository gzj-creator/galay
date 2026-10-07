/**
 * @file e2_echo.cc
 * @brief Echo RPC客户端示例（四种调用模式）
 *
 * @details 演示 unary / client_stream / server_stream / bidi 四种模式的 echo 调用。
 *
 * 使用方法:
 *   ./e2_echo [host] [port]
 *
 * 示例:
 *   ./e2_echo 127.0.0.1 9000
 */

#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <atomic>
#include <iostream>
#include <thread>
#include <string>
#include <string_view>
#include <cstdlib>

using namespace galay::rpc;
using namespace galay::kernel;

namespace {

std::atomic<bool> g_ok{true};

const char* call_mode_to_string(RpcCallMode mode) {
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

template<typename AwaitResult>
Task<void> call_echo_with_mode(std::string_view title,
                           RpcCallMode expected_mode,
                           const std::string& payload,
                           AwaitResult result) {
    std::cout << "=== " << title << " ===\n";
    std::cout << "Input: " << payload << "\n";

    if (!result) {
        std::cerr << "Task error: " << result.error().message() << "\n\n";
        g_ok.store(false, std::memory_order_release);
        co_return;
    }

    RpcCallResult call_result = std::move(result.value());
    if (!call_result.has_value()) {
        std::cerr << "RPC error: " << rpc_error_code_to_string(call_result.error().code()) << "\n\n";
        g_ok.store(false, std::memory_order_release);
        co_return;
    }

    if (!call_result.value().has_value()) {
        std::cerr << "No response received\n\n";
        g_ok.store(false, std::memory_order_release);
        co_return;
    }

    const auto& response = call_result.value().value();
    if (!response.is_ok()) {
        std::cerr << "RPC error: " << rpc_error_code_to_string(response.error_code()) << "\n\n";
        g_ok.store(false, std::memory_order_release);
        co_return;
    }

    std::string output(response.payload().begin(), response.payload().end());
    std::cout << "Output: " << output << "\n";
    std::cout << "Response mode: " << call_mode_to_string(response.call_mode())
              << ", end_of_stream=" << (response.end_of_stream() ? "true" : "false") << "\n";

    if (response.call_mode() != expected_mode) {
        std::cerr << "Mode mismatch: expected=" << call_mode_to_string(expected_mode)
                  << ", actual=" << call_mode_to_string(response.call_mode()) << "\n";
        g_ok.store(false, std::memory_order_release);
    }

    std::cout << "\n";
    co_return;
}

} // namespace

Task<void> run_client(Runtime& runtime, const std::string& host, uint16_t port) {
    (void)runtime;
    std::cout << "Connecting to " << host << ":" << port << "...\n";

    RpcClient client;

    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "Failed to connect: " << connect_result.error().message() << "\n";
        g_ok.store(false, std::memory_order_release);
        co_return;
    }

    std::cout << "Connected!\n\n";

    const std::string payload = "Hello, 4-mode RPC World!";

    co_await call_echo_with_mode("Unary Echo",
                              RpcCallMode::UNARY,
                              payload,
                              co_await client.call("EchoService", "echo", payload));

    co_await call_echo_with_mode("Client Streaming Echo (single frame)",
                              RpcCallMode::CLIENT_STREAMING,
                              payload,
                              co_await client.call_client_stream_frame("EchoService",
                                                                    "echo",
                                                                    payload.data(),
                                                                    payload.size(),
                                                                    true));

    co_await call_echo_with_mode("Server Streaming Echo (single response frame)",
                              RpcCallMode::SERVER_STREAMING,
                              payload,
                              co_await client.call_server_stream_request("EchoService",
                                                                      "echo",
                                                                      payload.data(),
                                                                      payload.size()));

    co_await call_echo_with_mode("Bidi Streaming Echo (single frame)",
                              RpcCallMode::BIDI_STREAMING,
                              payload,
                              co_await client.call_bidi_stream_frame("EchoService",
                                                                  "echo",
                                                                  payload.data(),
                                                                  payload.size(),
                                                                  true));

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

    std::cout << "=== Echo RPC Client Example (4 Modes) ===\n\n";

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(1).build();
    runtime.start();

    auto* scheduler = runtime.get_next_io_scheduler();
    (void)schedule_task(scheduler, run_client(runtime, host, port));

    std::this_thread::sleep_for(std::chrono::seconds(4));

    runtime.stop();

    return g_ok.load(std::memory_order_acquire) ? 0 : 1;
}
