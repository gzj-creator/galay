/**
 * @file t2_server.cpp
 * @brief RPC服务端测试
 */

#include "result_writer.h"
#include "galay-rpc/kernel/rpc_server.h"
#include "galay-rpc/kernel/rpc_service.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>

using namespace galay::rpc;
using namespace galay::kernel;

/**
 * @brief Echo服务实现
 */
class EchoService : public RpcService {
public:
    EchoService() : RpcService("EchoService") {
        registerMethod("echo", &EchoService::echo);
        registerMethod("uppercase", &EchoService::uppercase);
    }

    Coroutine echo(RpcContext& ctx) {
        auto& req = ctx.request();
        ctx.setPayload(req.payloadView());
        co_return;
    }

    Coroutine uppercase(RpcContext& ctx) {
        auto& req = ctx.request();
        std::string data(req.payload().begin(), req.payload().end());
        for (auto& c : data) {
            c = std::toupper(c);
        }
        ctx.setPayload(data);
        co_return;
    }
};

/**
 * @brief 计算服务实现
 */
class CalcService : public RpcService {
public:
    CalcService() : RpcService("CalcService") {
        registerMethod("add", &CalcService::add);
    }

    Coroutine add(RpcContext& ctx) {
        auto& payload = ctx.request().payload();
        if (payload.size() >= 8) {
            int32_t a, b;
            std::memcpy(&a, payload.data(), 4);
            std::memcpy(&b, payload.data() + 4, 4);
            int32_t result = a + b;
            ctx.setPayload(reinterpret_cast<char*>(&result), sizeof(result));
        } else {
            ctx.setError(RpcErrorCode::INVALID_REQUEST);
        }
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

    std::cout << "Starting RPC Server on port " << port << "...\n";

    // 创建服务
    auto echoService = std::make_shared<EchoService>();
    auto calcService = std::make_shared<CalcService>();

    // 启动服务器
    auto server = RpcServerBuilder()
        .host("0.0.0.0")
        .port(port)
        .ioSchedulerCount(2)
        .computeSchedulerCount(1)
        .build();
    server.registerService(echoService);
    server.registerService(calcService);
    server.start();

    std::cout << "RPC Server started. Press Ctrl+C to stop.\n";
    std::cout << "Registered services:\n";
    std::cout << "  - EchoService: echo, uppercase\n";
    std::cout << "  - CalcService: add\n";

    // 等待停止信号
    while (g_running.load() && server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Stopping server...\n";
    server.stop();
    std::cout << "Server stopped.\n";

    return 0;
}
