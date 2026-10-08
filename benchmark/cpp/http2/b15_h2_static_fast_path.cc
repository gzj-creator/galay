/**
 * @file b15_h2_static_fast_path.cc
 * @brief HTTP/2 静态响应 fast path h2load 服务端
 *
 * 使用方法:
 *   ./benchmark_http2_h2_static_fast_path [port] [io_threads] [max_streams] [debug]
 *   默认: 9080 4 1000 0
 */

#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-http2/server/http2_server.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace galay::http2;
using namespace galay::kernel;

namespace {

volatile bool g_running = true;
bool g_debug_log = false;
std::atomic<int64_t> g_fallback_requests{0};
const std::string kSmallBody(1024, 's');
std::filesystem::path g_static_root;

void signal_handler(int)
{
    g_running = false;
}

Task<void> fallback_active_handler(Http2ConnContext& ctx)
{
    while (true) {
        auto streams = co_await ctx.get_active_streams(64);
        if (!streams) {
            break;
        }

        for (auto& stream : *streams) {
            auto events = stream->take_events();
            if (!has_http2_stream_event(events, Http2StreamEvent::RequestComplete)) {
                continue;
            }

            g_fallback_requests.fetch_add(1, std::memory_order_relaxed);
            stream->send_headers(
                Http2Headers().status(404).content_type("text/plain").content_length(0),
                true,
                true);
        }
    }
    co_return;
}

void write_file(const std::filesystem::path& path, size_t size, char fill)
{
    std::ofstream out(path, std::ios::binary);
    out << std::string(size, fill);
}

} // namespace

int main(int argc, char* argv[])
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    uint16_t port = 9080;
    int io_threads = 4;
    uint32_t max_streams = 1000;
    int debug_log = 0;

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) io_threads = std::atoi(argv[2]);
    if (argc > 3) max_streams = static_cast<uint32_t>(std::atoi(argv[3]));
    if (argc > 4) debug_log = std::atoi(argv[4]);
    g_debug_log = debug_log > 0;

    std::cout << "========================================\n";
    std::cout << "H2c Static Fast Path Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "Max Concurrent Streams: " << max_streams << "\n";
    std::cout << "Static Route: GET/HEAD /echo -> 200 empty body\n";
    std::cout << "Static Route: GET/HEAD /small -> 200 1KB body\n";
    std::cout << "Static Files: GET/HEAD /files/{0b,1kb,16kb,128kb,1mb}.bin\n";
    std::cout << "Debug Log: " << (g_debug_log ? "ON" : "OFF") << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        g_static_root = std::filesystem::temp_directory_path() /
            ("galay-h2-static-bench-" + std::to_string(::getpid()));
        std::filesystem::create_directories(g_static_root);
        write_file(g_static_root / "0b.bin", 0, '0');
        write_file(g_static_root / "1kb.bin", 1024, '1');
        write_file(g_static_root / "16kb.bin", 16 * 1024, '6');
        write_file(g_static_root / "128kb.bin", 128 * 1024, '8');
        write_file(g_static_root / "1mb.bin", 1024 * 1024, 'm');

        H2cServer server(H2cServerBuilder<>()
            .host("0.0.0.0")
            .port(port)
            .io_scheduler_count(static_cast<size_t>(io_threads))
            .parallel_scheduler_count(0)
            .max_concurrent_streams(max_streams)
            .initial_window_size(65535)
            .static_response("/echo", H2StaticResponse{
                .status = 200,
                .content_type = "text/plain",
                .body = "",
            })
            .static_response("/small", H2StaticResponse{
                .status = 200,
                .content_type = "text/plain",
                .body = kSmallBody,
            })
            .static_files("/files", H2StaticFileConfig{
                .root = g_static_root,
                .small_file_threshold = 1024 * 1024,
            })
            .active_conn_handler(fallback_active_handler)
            .build_config());
        if (const auto started = server.start(); !started) {
            std::cerr << "Server startup failed: " << started.error().message << '\n';
        }

        std::cout << "Server started successfully!\n";
        std::cout << "Runtime Config: io=" << server.get_runtime().get_io_scheduler_count()
                  << " parallel=" << server.get_runtime().get_parallel_scheduler_count()
                  << " (configured io=" << io_threads << " parallel=0)\n";
        std::cout << "Waiting for requests...\n\n";

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (g_debug_log) {
                auto fallback = g_fallback_requests.exchange(0, std::memory_order_relaxed);
                if (fallback > 0) {
                    std::cerr << "[static-fast-path] fallback requests=" << fallback << "\n";
                }
            }
        }

        std::cout << "\nShutting down...\n";
        server.stop();
        std::filesystem::remove_all(g_static_root);
        std::cout << "Server stopped.\n";
    } catch (const std::exception& e) {
        if (!g_static_root.empty()) {
            std::filesystem::remove_all(g_static_root);
        }
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
