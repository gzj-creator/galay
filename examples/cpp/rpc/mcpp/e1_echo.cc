/**
 * @file e1_echo.cc
 * @brief Echo RPC服务端示例（C++23 import 版本）
 */

import galay.rpc;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace galay::rpc;
using namespace galay::kernel;

class EchoService : public RpcService {
public:
    EchoService() : RpcService("EchoService") {
        register_method("echo", &EchoService::echo);
        register_client_streaming_method("echo", &EchoService::echo);
        register_server_streaming_method("echo", &EchoService::echo);
        register_bidi_streaming_method("echo", &EchoService::echo);

        register_method("reverse", &EchoService::reverse);
        register_method("length", &EchoService::length);
    }

    Task<void> echo(RpcContext& ctx) {
        auto& req = ctx.request();
        ctx.set_payload(req.payload_view());
        co_return;
    }

    Task<void> reverse(RpcContext& ctx) {
        auto& payload = ctx.request().payload();
        std::string data(payload.begin(), payload.end());
        std::reverse(data.begin(), data.end());
        ctx.set_payload(data);
        co_return;
    }

    Task<void> length(RpcContext& ctx) {
        auto& payload = ctx.request().payload();
        uint32_t len = static_cast<uint32_t>(payload.size());
        ctx.set_payload(reinterpret_cast<char*>(&len), sizeof(len));
        co_return;
    }
};

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#if defined(SIGPIPE)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    uint16_t port = 9000;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    std::cout << "=== Echo RPC Server Example (import) ===\n\n";

    EchoService echoService;

    auto server = RpcServerBuilder()
        .host("0.0.0.0")
        .port(port)
        .build();
    auto registered = server.register_service(echoService);
    if (!registered.has_value()) {
        std::cerr << "Failed to register service: " << registered.error().message() << "\n";
        return 1;
    }
    auto started = server.start();
    if (!started.has_value()) {
        std::cerr << "Failed to start RPC server: " << started.error().message() << "\n";
        return 1;
    }

    std::cout << "Server listening on port " << port << "\n";
    std::cout << "Available methods:\n";
    std::cout << "  - EchoService.echo(data) [unary/client_stream/server_stream/bidi] -> data\n";
    std::cout << "  - EchoService.reverse(data) -> reversed data\n";
    std::cout << "  - EchoService.length(data) -> length (uint32)\n";
    std::cout << "\nPress Ctrl+C to stop.\n";

    while (g_running.load() && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "\nServer stopped.\n";

    return 0;
}
