/**
 * @file echo_client.cc
 * @brief Echo 客户端最小实践示例
 * @details 演示如何使用 HttpClient 发送请求到 Echo 服务器
 */

#include "http/client/http_client.h"
#include "kernel/kernel/runtime.h"
#include <iostream>
#include <map>
#include <string>

using namespace galay::http;
using namespace galay::kernel;

// 发送 Echo 请求的协程
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline))
#endif
Task<bool> sendEchoRequest(const std::string& url, const std::string& message) {
    std::cout << "Connecting to " << url << "...\n";

    // 创建 HttpClient 并连接
    auto client = HttpClientBuilder().build();
    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        std::cerr << "Failed to connect to server: " << connect_result.error().message() << "\n";
        co_return false;
    }

    std::cout << "Connected to server successfully\n";
    std::cout << "Sending request: POST " << client.url().path << "\n";
    std::cout << "Request body: " << message << "\n";
    auto socket_result = client.socket();
    if (!socket_result) {
        (void)co_await client.close();
        co_return false;
    }
    HttpSession session(socket_result.value().get());
    const std::string request_path = client.url().path;
    std::map<std::string, std::string> headers{
        {"Host", client.url().host + ":" + std::to_string(client.url().port)},
        {"Connection", "close"}
    };

    // 使用 HttpClient 的 post 方法发送请求并接收响应
    auto result = co_await session.post(request_path, message, "text/plain", headers);
    if (!result) {
        std::cerr << "Failed to send/receive: " << result.error().message() << "\n";
        co_return false;
    }

    auto response_opt = result.value();
    if (!response_opt.has_value()) {
        std::cerr << "Request incomplete\n";
        co_return false;
    }

    auto& response = response_opt.value();

    // 打印响应
    std::cout << "Response received:\n";
    std::cout << "  Status: " << static_cast<int>(response.header().code())
              << " " << httpStatusCodeToString(response.header().code()) << "\n";
    std::cout << "  Body: " << response.getBodyStr() << "\n";

    // GCC13 协程在复杂析构路径上可能触发 ICE，这里依赖析构关闭连接。
    std::cout << "Connection closed\n";

    co_return true;
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    std::string url = "http://127.0.0.1:8080/echo";
    std::string message = "Hello, Echo Server!";

    if (argc > 1) {
        url = argv[1];
    }
    if (argc > 2) {
        message = argv[2];
    }

    std::cout << "========================================\n";
    std::cout << "Echo Client Example\n";
    std::cout << "========================================\n";
    std::cout << "URL: " << url << "\n";
    std::cout << "Message: " << message << "\n";
    std::cout << "========================================\n\n";

    try {
        // 创建 Runtime
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        runtime.start();

        std::cout << "Runtime started\n";

        auto join = runtime.spawn(sendEchoRequest(url, message));
        bool ok = false;
        if (join) {
            auto result = join->join();
            ok = result && result.value();
        }

        // 停止 Runtime
        runtime.stop();
        std::cout << "Runtime stopped\n";

        if (!ok) {
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "Client error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
