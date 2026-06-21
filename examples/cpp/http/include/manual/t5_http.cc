/**
 * @file test_http_server.cc
 * @brief HTTP Server 测试
 */

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <galay/cpp/galay-http/server/http_server.h>
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-http/protoc/http_response.h>
#include <galay/cpp/galay-http/builder/http_builder.h>

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/io_uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::http;
using namespace galay::kernel;

std::atomic<bool> g_server_running{false};
std::atomic<int> g_request_count{0};

namespace {

constexpr const char* kApiAllowMethods = "GET, POST, PUT, DELETE, HEAD, OPTIONS, PATCH, TRACE";

int parseDelaySeconds(const std::string& uri) {
    constexpr std::string_view prefix = "/delay/";
    if (!uri.starts_with(prefix)) {
        return 0;
    }
    try {
        return std::max(0, std::stoi(uri.substr(prefix.size())));
    } catch (...) {
        return 0;
    }
}

std::string buildApiDataBody(std::string_view method_name,
                             std::string_view uri,
                             size_t body_size) {
    return std::string("{\"method\":\"")
        + std::string(method_name)
        + "\",\"uri\":\""
        + std::string(uri)
        + "\",\"body_size\":"
        + std::to_string(body_size)
        + "}";
}

Task<void> sendRawPayload(HttpConn& conn, std::string payload) {
    auto& socket = conn.getSocket();
    size_t sent = 0;
    while (sent < payload.size()) {
        auto result = co_await socket.send(payload.data() + sent, payload.size() - sent);
        if (!result) {
            co_return;
        }
        sent += result.value();
    }
    co_return;
}

}  // namespace

// HTTP请求处理器协程
Task<void> handleRequest(HttpConn conn) {
    auto reader = conn.getReader();
    auto writer = conn.getWriter();

    while (true) {
        // 读取HTTP请求
        HttpRequest request;
        bool requestComplete = false;

        while (!requestComplete) {
            auto result = co_await reader.getRequest(request);

            if (!result) {
                auto& error = result.error();
                if (error.code() == kConnectionClose) {
                } else {
                }
                co_await conn.close();
                co_return;
            }

            requestComplete = result.value();
        }

        int req_no = g_request_count.fetch_add(1) + 1;


        // 根据不同的路径返回不同的内容
        HttpStatusCode code = HttpStatusCode::OK_200;
        std::string content_type = "text/html; charset=utf-8";
        std::string body;
        std::string uri = request.header().uri();
        const auto method = request.header().method();

        if (uri == "/disconnect") {
            co_await conn.close();
            co_return;
        } else if (uri == "/partial") {
            // Advertise a longer body than we actually send so client timeout paths
            // can observe an incomplete response on a live connection.
            std::string partial =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 1024\r\n"
                "Connection: keep-alive\r\n"
                "\r\n"
                "partial";
            co_await sendRawPayload(conn, std::move(partial));
            continue;
        } else if (uri.starts_with("/delay/")) {
            const int delay_seconds = parseDelaySeconds(uri);
            std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));
            content_type = "text/plain; charset=utf-8";
            body = "Delayed " + std::to_string(delay_seconds) + " second(s)";
        } else if (uri == "/" || uri == "/index.html") {
            body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Galay HTTP Server</title>
</head>
<body>
    <h1>Welcome to Galay HTTP Server!</h1>
    <p>This is a test page.</p>
    <ul>
        <li><a href="/hello">Hello Page</a></li>
        <li><a href="/test">Test Page</a></li>
        <li><a href="/api/info">API Info</a></li>
    </ul>
</body>
</html>)";
        } else if (uri == "/hello") {
            body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Hello</title>
</head>
<body>
    <h1>Hello from Galay HTTP!</h1>
    <p><a href="/">Back to Home</a></p>
</body>
</html>)";
        } else if (uri == "/test") {
            body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Test</title>
</head>
<body>
    <h1>Test Page</h1>
    <p>This is a test page for Galay HTTP Server.</p>
    <p><a href="/">Back to Home</a></p>
</body>
</html>)";
        } else if (uri == "/api/info") {
            content_type = "application/json";
            body = R"({
    "server": "galay-http",
    "version": "2.0.0",
    "status": "running",
    "timestamp": ")" + std::to_string(std::time(nullptr)) + R"("
})";
        } else if (uri == "/api/data" || uri == "/api/data/1") {
            content_type = "application/json";
            switch (method) {
                case HttpMethod::GET:
                case HttpMethod::POST:
                case HttpMethod::PUT:
                case HttpMethod::PATCH:
                    body = buildApiDataBody(
                        httpMethodToString(method),
                        uri,
                        request.bodyStr().size());
                    break;
                case HttpMethod::DELETE:
                    code = HttpStatusCode::NoContent_204;
                    content_type.clear();
                    body.clear();
                    break;
                case HttpMethod::HEAD:
                    body.clear();
                    break;
                case HttpMethod::OPTIONS:
                    code = HttpStatusCode::NoContent_204;
                    content_type.clear();
                    body.clear();
                    break;
                case HttpMethod::TRACE:
                    content_type = "text/plain; charset=utf-8";
                    body = std::string("TRACE ") + uri;
                    break;
                default:
                    code = HttpStatusCode::MethodNotAllowed_405;
                    content_type = "text/plain; charset=utf-8";
                    body = "Method Not Allowed";
                    break;
            }
        } else {
            code = HttpStatusCode::NotFound_404;
            body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>404 Not Found</title>
</head>
<body>
    <h1>404 Not Found</h1>
    <p>The requested URL was not found on this server.</p>
    <p><a href="/">Back to Home</a></p>
</body>
</html>)";
        }

        bool keep_alive = true;
        std::string conn_hdr = request.header().headerPairs().getValue("Connection");
        if (!conn_hdr.empty()) {
            for (auto& c : conn_hdr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (conn_hdr == "close") {
                keep_alive = false;
            }
        }

        // 发送响应
        auto response_builder = Http1_1ResponseBuilder()
            .status(code)
            .header("Server", GALAY_SERVER)
            .header("Connection", keep_alive ? "keep-alive" : "close");

        if (!content_type.empty()) {
            response_builder.header("Content-Type", content_type);
        }
        if (uri == "/api/data" || uri == "/api/data/1") {
            response_builder.header("Allow", kApiAllowMethods);
        }
        if (method != HttpMethod::HEAD && !body.empty()) {
            response_builder.body(std::move(body));
        }

        auto response = response_builder.buildMove();

        auto sendResult = co_await writer.sendResponse(response);
        if (!sendResult) {
            keep_alive = false;
        } else {
        }

        if (!keep_alive) {
            break;
        }
    }

    co_await conn.close();
    co_return;
}

int main() {

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    // 配置并启动服务器
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(8080)
        .backlog(128)
        .build());

    g_server_running = true;

    // 运行服务器（阻塞）
    server.start(handleRequest);
    while (g_server_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    server.stop();
#else
    return 1;
#endif

    return 0;
}
