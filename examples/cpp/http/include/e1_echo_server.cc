/**
 * @file echo_server.cc
 * @brief Echo 服务器最小实践示例
 * @details 演示如何使用 HttpServer 和 HttpRouter 创建一个简单的 Echo 服务器
 */

#include <galay/cpp/galay-http/server/http_server.h>
#include <galay/cpp/galay-http/server/http_router.h>
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-http/builder/http_builder.h>
#include <iostream>
#include <string>

using namespace galay::http;
using namespace galay::kernel;

// Echo 处理器：返回客户端发送的内容
Task<void> echoHandler(HttpConn& conn, HttpRequest req) {
    // 获取请求体
    std::string requestBody = req.getBodyStr();

    // 使用 Builder 构造响应 - 简洁优雅！
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "Galay-HTTP-Echo/1.0")
        .text(requestBody.empty() ? "Echo: (empty body)" : "Echo: " + requestBody)
        .build();

    // 发送响应
    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);
    if (!result) {
        std::cerr << "Failed to send response: " << result.error().message() << "\n";
    }

    // 不要在handler中关闭连接，让HttpServer的路由处理器根据Keep-Alive决定
    co_return;
}

// 主页处理器
Task<void> indexHandler(HttpConn& conn, HttpRequest req) {
    std::string body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Echo Server</title>
</head>
<body>
    <h1>Welcome to Echo Server</h1>
    <p>Send a POST request to <code>/echo</code> to test the echo functionality.</p>
    <h2>Example:</h2>
    <pre>curl -X POST http://localhost:8080/echo -d "Hello, World!"</pre>
</body>
</html>)";

    // 使用 Builder 构造 HTML 响应
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "Galay-HTTP-Echo/1.0")
        .html(body)
        .build();

    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);
    if (!result) {
        std::cerr << "Failed to send response: " << result.error().message() << "\n";
    }

    // 不要在handler中关闭连接，让HttpServer的路由处理器根据Keep-Alive决定
    co_return;
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    std::cout << "========================================\n";
    std::cout << "Echo Server Example\n";
    std::cout << "========================================\n";
    std::cout << "Server will listen on port: " << port << "\n";
    std::cout << "========================================\n\n";

    try {
        // 创建路由器
        HttpRouter router;

        // 注册路由
        router.addHandler<HttpMethod::GET>("/", indexHandler);
        router.addHandler<HttpMethod::POST>("/echo", echoHandler);

        // 创建并启动服务器
        HttpServer server(HttpServerBuilder()
            .host("0.0.0.0")
            .port(static_cast<uint16_t>(port))
            .backlog(128)
            .build());

        std::cout << "========================================\n";
        std::cout << "Server is running on http://0.0.0.0:" << port << "\n";
        std::cout << "========================================\n";
        std::cout << "Test URLs:\n";
        std::cout << "  - http://localhost:" << port << "/\n";
        std::cout << "  - curl -X POST http://localhost:" << port << "/echo -d \"Hello\"\n";
        std::cout << "========================================\n";
        std::cout << "Press Ctrl+C to stop the server\n";
        std::cout << "========================================\n\n";

        // 使用新的 API：将 Router 移动到 Server 内部
        server.start(std::move(router));

        // 保持服务器运行
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
