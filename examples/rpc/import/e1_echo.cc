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
        registerMethod("echo", &EchoService::echo);
        registerClientStreamingMethod("echo", &EchoService::echo);
        registerServerStreamingMethod("echo", &EchoService::echo);
        registerBidiStreamingMethod("echo", &EchoService::echo);

        registerMethod("reverse", &EchoService::reverse);
        registerMethod("length", &EchoService::length);
    }

    Coroutine echo(RpcContext& ctx) {
        auto& req = ctx.request();
        ctx.setPayload(req.payloadView());
        co_return;
    }

    Coroutine reverse(RpcContext& ctx) {
        auto& payload = ctx.request().payload();
        std::string data(payload.begin(), payload.end());
        std::reverse(data.begin(), data.end());
        ctx.setPayload(data);
        co_return;
    }

    Coroutine length(RpcContext& ctx) {
        auto& payload = ctx.request().payload();
        uint32_t len = static_cast<uint32_t>(payload.size());
        ctx.setPayload(reinterpret_cast<char*>(&len), sizeof(len));
        co_return;
    }
};

std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#if defined(SIGPIPE)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    uint16_t port = 9000;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    std::cout << "=== Echo RPC Server Example (import) ===\n\n";

    auto echoService = std::make_shared<EchoService>();

    auto server = RpcServerBuilder()
        .host("0.0.0.0")
        .port(port)
        .build();
    server.registerService(echoService);
    server.start();

    std::cout << "Server listening on port " << port << "\n";
    std::cout << "Available methods:\n";
    std::cout << "  - EchoService.echo(data) [unary/client_stream/server_stream/bidi] -> data\n";
    std::cout << "  - EchoService.reverse(data) -> reversed data\n";
    std::cout << "  - EchoService.length(data) -> length (uint32)\n";
    std::cout << "\nPress Ctrl+C to stop.\n";

    while (g_running.load() && server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "\nServer stopped.\n";

    return 0;
}
