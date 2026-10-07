/**
 * @file e1_echo.cc
 * @brief Echo RPC服务端示例
 *
 * @details 演示如何创建一个简单的RPC服务端
 *
 * 使用方法:
 *   ./e1_echo [port]
 *
 * 示例:
 *   ./e1_echo 9000
 */

#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <iostream>
#include <csignal>
#include <atomic>
#include <algorithm>

using namespace galay::rpc;
using namespace galay::kernel;

/**
 * @brief Echo服务实现
 */
class EchoService : public RpcService {
public:
    EchoService() : RpcService("EchoService") {
        // 同名 echo 方法按调用模式路由
        register_method("echo", &EchoService::echo);
        register_client_streaming_method("echo", &EchoService::echo);
        register_server_streaming_method("echo", &EchoService::echo);
        register_bidi_streaming_method("echo", &EchoService::echo);

        // 其他一元方法
        register_method("reverse", &EchoService::reverse);
        register_method("length", &EchoService::length);
    }

    /**
     * @brief Echo方法 - 原样返回输入
     */
    Task<void> echo(RpcContext& ctx) {
        auto& req = ctx.request();
        ctx.set_payload(req.payload_view());
        co_return;
    }

    /**
     * @brief Reverse方法 - 反转字符串
     */
    Task<void> reverse(RpcContext& ctx) {
        auto& payload = ctx.request().payload();
        std::string data(payload.begin(), payload.end());
        std::reverse(data.begin(), data.end());
        ctx.set_payload(data);
        co_return;
    }

    /**
     * @brief Length方法 - 返回字符串长度
     */
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

    std::cout << "=== Echo RPC Server Example ===\n\n";

    // 创建服务
    EchoService echoService;

    // 创建并启动服务器
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

    // 等待停止信号
    while (g_running.load() && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "\nServer stopped.\n";

    return 0;
}
