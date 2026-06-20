/**
 * @file b15_h2_static_fast_path.cc
 * @brief HTTP/2 静态响应 fast path h2load 服务端
 *
 * 使用方法:
 *   ./benchmark_http2_h2_static_fast_path [port] [io_threads] [max_streams] [debug]
 *   默认: 9080 4 1000 0
 */

#include "galay-http2/server/http2_server.h"

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

void signalHandler(int)
{
    g_running = false;
}

Task<void> fallbackActiveHandler(Http2ConnContext& ctx)
{
    while (true) {
        auto streams = co_await ctx.getActiveStreams(64);
        if (!streams) {
            break;
        }

        for (auto& stream : *streams) {
            auto events = stream->takeEvents();
            if (!hasHttp2StreamEvent(events, Http2StreamEvent::RequestComplete)) {
                continue;
            }

            g_fallback_requests.fetch_add(1, std::memory_order_relaxed);
            stream->sendHeaders(
                Http2Headers().status(404).contentType("text/plain").contentLength(0),
                true,
                true);
        }
    }
    co_return;
}

void writeFile(const std::filesystem::path& path, size_t size, char fill)
{
    std::ofstream out(path, std::ios::binary);
    out << std::string(size, fill);
}

} // namespace

int main(int argc, char* argv[])
{
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

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    try {
        g_static_root = std::filesystem::temp_directory_path() /
            ("galay-h2-static-bench-" + std::to_string(::getpid()));
        std::filesystem::create_directories(g_static_root);
        writeFile(g_static_root / "0b.bin", 0, '0');
        writeFile(g_static_root / "1kb.bin", 1024, '1');
        writeFile(g_static_root / "16kb.bin", 16 * 1024, '6');
        writeFile(g_static_root / "128kb.bin", 128 * 1024, '8');
        writeFile(g_static_root / "1mb.bin", 1024 * 1024, 'm');

        H2cServer server(H2cServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .ioSchedulerCount(static_cast<size_t>(io_threads))
            .computeSchedulerCount(0)
            .maxConcurrentStreams(max_streams)
            .initialWindowSize(65535)
            .staticResponse("/echo", H2StaticResponse{
                .status = 200,
                .content_type = "text/plain",
                .body = "",
            })
            .staticResponse("/small", H2StaticResponse{
                .status = 200,
                .content_type = "text/plain",
                .body = kSmallBody,
            })
            .staticFiles("/files", H2StaticFileConfig{
                .root = g_static_root,
                .small_file_threshold = 1024 * 1024,
            })
            .activeConnHandler(fallbackActiveHandler)
            .build());
        server.start();

        std::cout << "Server started successfully!\n";
        std::cout << "Runtime Config: io=" << server.getRuntime().getIOSchedulerCount()
                  << " compute=" << server.getRuntime().getComputeSchedulerCount()
                  << " (configured io=" << io_threads << " compute=0)\n";
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
