/**
 * @file E5-HttpsServer.cc
 * @brief HTTPS 服务器示例
 * @details 演示如何使用 HttpsServer 创建一个支持 Keep-Alive 的 HTTPS 服务器
 */

#include "galay-http/server/http_server.h"
#include "galay-http/builder/http_builder.h"
#include <iostream>
#include <csignal>

using namespace galay::http;
using namespace galay::kernel;

#ifdef GALAY_SSL_FEATURE_ENABLED

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_requests{0};

void signalHandler(int) {
    g_running = false;
}

// HTTPS 请求处理器 (支持 Keep-Alive)
Task<void> httpsHandler(HttpConnImpl<galay::ssl::SslSocket> conn) {
    auto reader = conn.getReader();
    auto writer = conn.getWriter();

    while (true) {
        HttpRequest request;

        // 读取请求
        while (true) {
            auto r = co_await reader.getRequest(request);
            if (!r) {
                auto close_result = co_await conn.close();
                if (!close_result) {
                    std::cerr << "Failed to close connection: " << close_result.error().message() << "\n";
                }
                co_return;
            }
            if (r.value()) break;
        }

        g_requests++;

        // 检查 keep-alive
        bool keep_alive = true;
        auto conn_header = request.header().headerPairs().getValue("Connection");
        if (conn_header == "close") {
            keep_alive = false;
        }

        // 根据 URI 返回不同响应
        std::string uri = request.header().uri();
        std::string body = request.getBodyStr();
        std::string html = R"(<!DOCTYPE html>
<html>
<head><meta charset="UTF-8"><title>HTTPS Server</title></head>
<body>
    <h1>Welcome to HTTPS Server</h1>
    <p>This is a secure connection using TLS.</p>
    <h2>Test:</h2>
    <pre>curl -k https://localhost:8443/echo -d "Hello"</pre>
</body>
</html>)";
        HttpResponse response = (uri == "/echo")
            ? Http1_1ResponseBuilder::ok()
                .header("Server", "Galay-HTTPS/1.0")
                .header("Connection", keep_alive ? "keep-alive" : "close")
                .text(body.empty() ? "Echo: (empty)" : "Echo: " + body)
                .build()
            : Http1_1ResponseBuilder::ok()
                .header("Server", "Galay-HTTPS/1.0")
                .header("Connection", keep_alive ? "keep-alive" : "close")
                .html(html)
                .build();

        // 发送响应
        auto result = co_await writer.sendResponse(response);
        if (!result) {
            std::cerr << "Failed to send response: " << result.error().message() << "\n";
        }

        if (!keep_alive) break;
        request = HttpRequest();
    }

    auto close_result = co_await conn.close();
    if (!close_result) {
        std::cerr << "Failed to close connection: " << close_result.error().message() << "\n";
    }
    co_return;
}

int main(int argc, char* argv[]) {
    int port = 8443;
    std::string cert_path = "test/test.crt";
    std::string key_path = "test/test.key";

    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) cert_path = argv[2];
    if (argc > 3) key_path = argv[3];

    std::cout << "========================================\n";
    std::cout << "HTTPS Server Example\n";
    std::cout << "========================================\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpsServer server(HttpsServerBuilder()
            .host("0.0.0.0")
            .port(static_cast<uint16_t>(port))
            .certPath(cert_path)
            .keyPath(key_path)
            .ioSchedulerCount(4)
            .build());

        std::cout << "Server running on https://0.0.0.0:" << port << "\n";
        std::cout << "Test: curl -k https://localhost:" << port << "/\n";
        std::cout << "Press Ctrl+C to stop\n";
        std::cout << "========================================\n";

        server.start(httpsHandler);

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nTotal requests: " << g_requests << "\n";
        server.stop();
        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
