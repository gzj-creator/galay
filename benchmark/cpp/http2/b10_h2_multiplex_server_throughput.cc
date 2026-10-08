/**
 * @file b10_h2mux.cc
 * @brief HTTP/2 多路复用 Echo 服务器压测程序
 * @details 与 B3 相同的 echo 逻辑，但配置更高的 max_concurrent_streams
 *          以支持单连接上大量并发流的压测场景
 *
 * 使用方法:
 *   ./B10-H2cMuxServer [port] [io_threads] [max_streams] [debug]
 *   默认: 9080 4 1000 0
 *
 * 压测命令:
 *   ./B11-H2cMuxClient localhost 9080 10 100 50
 */

#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-http2/server/http2_server.h>
#include <iostream>
#include <csignal>
#include <atomic>
#include <memory>

using namespace galay::http2;
using namespace galay::kernel;

static volatile bool g_running = true;
static bool g_debug_log = false;
static std::atomic<int> g_debug_logs{0};

static const std::string& encoded_echo_headers(size_t body_size) {
    static const std::string kEncoded0 = [] {
        HpackEncoder encoder;
        return encoder.encode_stateless({
            {":status", "200"},
            {"content-type", "text/plain"},
            {"content-length", "0"},
        });
    }();
    static const std::string kEncoded128 = [] {
        HpackEncoder encoder;
        return encoder.encode_stateless({
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
    dynamic_headers = encoder.encode_stateless({
        {":status", "200"},
        {"content-type", "text/plain"},
        {"content-length", std::to_string(body_size)},
    });
    return dynamic_headers;
}

static std::shared_ptr<const std::string> shared_encoded_echo_headers(size_t body_size) {
    static const auto kEncoded0 = std::make_shared<const std::string>(encoded_echo_headers(0));
    static const auto kEncoded128 = std::make_shared<const std::string>(encoded_echo_headers(128));

    if (body_size == 0) {
        return kEncoded0;
    }
    if (body_size == 128) {
        return kEncoded128;
    }
    return std::make_shared<const std::string>(encoded_echo_headers(body_size));
}

void signal_handler(int) {
    g_running = false;
}

Task<void> handle_active_conn(Http2ConnContext& ctx) {
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

            const size_t body_size = stream->request().body_size();
            const size_t body_chunk_count = stream->request().body_chunk_count();
            auto response_headers = shared_encoded_echo_headers(body_size);

            if (g_debug_log) {
                int idx = g_debug_logs.fetch_add(1);
                if (idx < 10) {
                    std::cerr << "[echo] recv body_len=" << body_size << "\n";
                }
            }

            if (body_chunk_count == 1) {
                stream->send_encoded_headers_and_data(
                    std::move(response_headers), stream->request().take_single_body_chunk(), true);
            } else if (body_chunk_count > 1) {
                stream->send_encoded_headers_and_data_chunks(
                    std::string(*response_headers), stream->request().take_body_chunks(), true);
            } else {
                stream->send_encoded_headers_and_data(
                    std::move(response_headers), std::string(), true);
            }
        }
    }
    co_return;
}

int main(int argc, char* argv[]) {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    uint16_t port = 9080;
    int io_threads = 4;
    uint32_t max_streams = 1000;
    int debug_log = 0;

    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) io_threads = std::atoi(argv[2]);
    if (argc > 3) max_streams = std::atoi(argv[3]);
    if (argc > 4) debug_log = std::atoi(argv[4]);

    if (debug_log > 0) {
    } else {
    }
    g_debug_log = (debug_log > 0);

    std::cout << "========================================\n";
    std::cout << "H2c Mux Server Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "Max Concurrent Streams: " << max_streams << "\n";
    std::cout << "Debug Log: " << (debug_log > 0 ? "ON" : "OFF") << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        H2cServer server(H2cServerBuilder<>()
            .host("0.0.0.0")
            .port(port)
            .io_scheduler_count(static_cast<size_t>(io_threads))
            .parallel_scheduler_count(0)
            .max_concurrent_streams(static_cast<uint32_t>(max_streams))
            .initial_window_size(65535)
            .active_conn_handler(handle_active_conn)
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
