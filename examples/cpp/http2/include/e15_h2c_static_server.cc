/**
 * @file E15-H2cStaticServer.cc
 * @brief h2c 静态响应/静态文件服务器示例
 *
 * 运行:
 *   ./example_http2_include_h2c_static_server [port] [static_root]
 *
 * 测试:
 *   curl --http2-prior-knowledge http://127.0.0.1:8080/healthz
 *   curl --http2-prior-knowledge http://127.0.0.1:8080/files/index.txt
 */

#include <galay/cpp/galay-http2/server/http2_server.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace galay::http2;
using namespace galay::kernel;

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int)
{
    g_running = false;
}

Task<void> fallback_active_handler(Http2ConnContext& ctx)
{
    while (true) {
        auto streams = co_await ctx.get_active_streams(16);
        if (!streams) {
            break;
        }
        for (auto& stream : *streams) {
            auto events = stream->take_events();
            if (!has_http2_stream_event(events, Http2StreamEvent::RequestComplete)) {
                continue;
            }
            stream->send_headers(
                Http2Headers().status(404).content_type("text/plain").content_length(0),
                true,
                true);
        }
    }
    co_return;
}

void ensure_example_files(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    const auto index = root / "index.txt";
    if (!std::filesystem::exists(index)) {
        std::ofstream(index) << "hello from h2c static server\n";
    }
}

} // namespace

int main(int argc, char* argv[])
{
    uint16_t port = 8080;
    std::filesystem::path static_root = "examples/cpp/http2/public";
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        static_root = argv[2];
    }

    ensure_example_files(static_root);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "========================================\n";
    std::cout << "H2c Static Server Example\n";
    std::cout << "========================================\n";
    std::cout << "Server: http://127.0.0.1:" << port << "\n";
    std::cout << "Static root: " << static_root << "\n";
    std::cout << "Static response: /healthz -> ok\n";
    std::cout << "Static files: /files/*\n";
    std::cout << "========================================\n";

    H2cServer server(H2cServerBuilder<>()
        .host("0.0.0.0")
        .port(port)
        .io_scheduler_count(2)
        .parallel_scheduler_count(0)
        .static_response("/healthz", H2StaticResponse{
            .status = 200,
            .content_type = "text/plain",
            .body = "ok",
        })
        .static_files("/files", H2StaticFileConfig{.root = static_root})
        .active_conn_handler(fallback_active_handler)
        .build_config());

    if (const auto started = server.start(); !started) {

        std::cerr << "Server startup failed: " << started.error().message << '\n';

    }
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.stop();
    return 0;
}
