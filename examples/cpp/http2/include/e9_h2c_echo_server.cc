/**
 * @file E9-H2cEchoServer.cc
 * @brief h2c (HTTP/2 over cleartext) Echo 服务器示例
 * @details 演示如何使用 H2cServer 创建一个 HTTP/2 Echo 服务器
 *
 * 测试方法:
 *   # 使用 curl (Prior Knowledge 模式)
 *   curl --http2-prior-knowledge -v http://localhost:8080/echo -d "Hello HTTP/2"
 *
 *   # 使用 nghttp
 *   nghttp -v http://localhost:8080/
 */

#include <galay/cpp/galay-http2/server/http2_server.h>
#include <iostream>
#include <csignal>

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_requests{0};

void signal_handler(int) {
    g_running = false;
}

// 处理单个流的请求
Task<void> handle_stream(Http2Stream::ptr stream) {
    g_requests++;

    // 读取完整请求（帧驱动）
    while (true) {
        auto frame_result = co_await stream->get_frame();
        if (!frame_result || !frame_result.value()) {
            co_return;
        }
        auto frame = std::move(frame_result.value());
        if ((frame->is_headers() || frame->is_data()) && frame->is_end_stream()) {
            break;
        }
    }
    auto& req = stream->request();


    // 构建响应（echo body）
    co_await stream->reply_header(
        Http2Headers().status(200).content_type("text/plain")
            .server("Galay-H2c-Echo/1.0").content_length(req.body.size()),
        req.body.empty());
    if (!req.body.empty()) {
        co_await stream->reply_data(req.take_single_body_chunk(), true);
    }

    co_return;
}

int main(int argc, char* argv[]) {

    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    std::cout << "========================================\n";
    std::cout << "H2c (HTTP/2 Cleartext) Echo Server Example\n";
    std::cout << "========================================\n";

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        H2cServer server(H2cServerBuilder()
            .host("0.0.0.0")
            .port(static_cast<uint16_t>(port))
            .io_scheduler_count(4)
            .max_concurrent_streams(100)
            .enable_push(false)
            .build());

        std::cout << "Server running on http://0.0.0.0:" << port << "\n";
        std::cout << "Test: curl --http2-prior-knowledge http://localhost:" << port << "/echo -d \"Hello\"\n";
        std::cout << "Press Ctrl+C to stop\n";
        std::cout << "========================================\n";

        server.start(handle_stream);

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
