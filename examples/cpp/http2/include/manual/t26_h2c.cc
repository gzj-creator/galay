/**
 * @file T26-H2cServer.cc
 * @brief H2c 服务器测试程序
 * @details 用于测试 H2c 服务器功能
 *
 * 使用方法:
 *   ./test/T26-H2cServer [port]
 *   默认端口: 9080
 */

#include <galay/cpp/galay-http2/server/http2_server.h>
#include <iostream>
#include <atomic>
#include <csignal>

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_request_count{0};

void signal_handler(int) {
    g_running = false;
}

/**
 * @brief 处理单个流的请求
 */
Task<void> handle_stream(Http2Stream::ptr stream) {
    g_request_count++;

    // New contract: frame-first request consumption.
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


    // 构造响应
    std::string resp_body = "Hello from H2c Test Server!\n";
    resp_body += "Request #" + std::to_string(g_request_count.load()) + "\n";
    resp_body += "Stream ID: " + std::to_string(stream->stream_id()) + "\n";

    co_await stream->reply_header(
        Http2Headers().status(200).content_type("text/plain").server("Galay-H2c-Test/1.0"),
        false);
    co_await stream->reply_data(resp_body, true);

    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    std::cout << "========================================\n";
    std::cout << "H2c Server Test\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "Test command: ./test/T25-H2cClient localhost " << port << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        H2cServer server(H2cServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .io_scheduler_count(4)
            .parallel_scheduler_count(0)
            .max_concurrent_streams(100)
            .initial_window_size(65535)
            .enable_push(false)
            .build());


        server.start(handle_stream);

        std::cout << "Server started successfully!\n\n";

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\n\nShutting down...\n";
        std::cout << "Total requests handled: " << g_request_count << "\n";

        server.stop();
        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
