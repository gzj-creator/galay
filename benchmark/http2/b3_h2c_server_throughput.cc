/**
 * @file b3_h2c.cc
 * @brief HTTP/2 Cleartext (h2c) Echo 服务器压测程序
 * @details 高性能 H2c Echo 服务器，移除统计功能，由客户端负责统计
 */

#include "http2/server/http2_server.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <memory>
using namespace galay::http2;
using namespace galay::kernel;

static volatile bool g_running = true;
static bool g_debug_log = false;
static std::atomic<int> g_debug_logs{0};

static const std::string& encodedEchoHeaders(size_t body_size) {
    static const std::string kEncoded0 = [] {
        HpackEncoder encoder;
        return encoder.encodeStateless({
            {":status", "200"},
            {"content-type", "text/plain"},
            {"content-length", "0"},
        });
    }();
    static const std::string kEncoded128 = [] {
        HpackEncoder encoder;
        return encoder.encodeStateless({
            {":status", "200"},
            {"content-type", "text/plain"},
            {"content-length", "128"},
        });
    }();

    if (body_size == 0) {
        return kEncoded0;
    }
    if (body_size == 128) {
        return kEncoded128;
    }

    static thread_local std::string dynamic_headers;
    HpackEncoder encoder;
    dynamic_headers = encoder.encodeStateless({
        {":status", "200"},
        {"content-type", "text/plain"},
        {"content-length", std::to_string(body_size)},
    });
    return dynamic_headers;
}

static std::shared_ptr<const std::string> sharedEncodedEchoHeaders(size_t body_size) {
    static const auto kEncoded0 = std::make_shared<const std::string>(encodedEchoHeaders(0));
    static const auto kEncoded128 = std::make_shared<const std::string>(encodedEchoHeaders(128));

    if (body_size == 0) {
        return kEncoded0;
    }
    if (body_size == 128) {
        return kEncoded128;
    }
    return std::make_shared<const std::string>(encodedEchoHeaders(body_size));
}

void signalHandler(int) {
    g_running = false;
}

Task<void> handleActiveConn(Http2ConnContext& ctx) {
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

            const size_t body_size = stream->request().bodySize();
            const size_t body_chunk_count = stream->request().bodyChunkCount();
            auto response_headers = sharedEncodedEchoHeaders(body_size);

            if (g_debug_log) {
                int idx = g_debug_logs.fetch_add(1);
                if (idx < 10) {
                    std::cerr << "[echo] recv body_len=" << body_size << "\n";
                }
            }

            if (body_chunk_count == 1) {
                stream->sendEncodedHeadersAndData(
                    std::move(response_headers), stream->request().takeSingleBodyChunk(), true);
            } else if (body_chunk_count > 1) {
                stream->sendEncodedHeadersAndDataChunks(
                    std::string(*response_headers), stream->request().takeBodyChunks(), true);
            } else {
                stream->sendEncodedHeadersAndData(
                    std::move(response_headers), std::string(), true);
            }
        }
    }
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9080;
    int io_threads = 4;
    int debug_log = 0;

    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    if (argc > 2) {
        io_threads = std::atoi(argv[2]);
    }
    if (argc > 3) {
        debug_log = std::atoi(argv[3]);
    }

    // 默认禁用日志以获得最佳性能，debug_log=1 时开启
    if (debug_log > 0) {
    } else {
    }
    g_debug_log = (debug_log > 0);

    std::cout << "========================================\n";
    std::cout << "HTTP/2 Cleartext (h2c) Server Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "Configured Compute Threads: 0\n";
    std::cout << "Debug Log: " << (debug_log > 0 ? "ON" : "OFF") << "\n";
    std::cout << "Test command: ./build/benchmark/b4_h2c localhost " << port << " <connections> <requests>\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        H2cServer server(H2cServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .ioSchedulerCount(static_cast<size_t>(io_threads))
            .computeSchedulerCount(0)
            .maxConcurrentStreams(1000)
            .initialWindowSize(65535)
            .activeConnHandler(handleActiveConn)
            .build());

        server.start();

        std::cout << "Server started successfully!\n";
        std::cout << "Runtime Config: io=" << server.getRuntime().getIOSchedulerCount()
                  << " compute=" << server.getRuntime().getComputeSchedulerCount()
                  << " (configured io=" << io_threads << " compute=0)\n";
        std::cout << "Waiting for requests...\n\n";

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nShutting down...\n";
        server.stop();
        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
