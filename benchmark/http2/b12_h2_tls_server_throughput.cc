/**
 * @file b12_h2.cc
 * @brief H2 (HTTP/2 over TLS) Echo 服务器压测程序
 */

#include "http2/server/http2_server.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <unordered_map>

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::http2;
using namespace galay::kernel;

static volatile bool g_running = true;
static constexpr size_t kBenchmarkEchoBodySize = 128;
static bool g_debug_log = false;
static bool g_debug_stats = false;
static std::atomic<uint64_t> g_active_conn_handlers_started{0};
static std::atomic<uint64_t> g_active_conn_handlers_finished{0};
static std::atomic<uint64_t> g_active_conn_handlers_finished_while_running{0};
static std::atomic<uint64_t> g_active_conn_handlers_without_request{0};
static std::atomic<uint64_t> g_active_stream_visits{0};
static std::atomic<uint64_t> g_active_stream_incomplete_visits{0};
static std::atomic<uint64_t> g_headers_ready_events{0};
static std::atomic<uint64_t> g_data_arrived_events{0};
static std::atomic<uint64_t> g_request_complete_events{0};
static std::atomic<uint64_t> g_conn_requests_lt_256{0};
static std::atomic<uint64_t> g_conn_requests_lt_512{0};
static std::atomic<uint64_t> g_conn_requests_lt_768{0};
static std::atomic<uint64_t> g_conn_requests_lt_1024{0};
static std::atomic<uint64_t> g_conn_requests_lt_1280{0};
static std::atomic<uint64_t> g_conn_requests_ge_1280{0};
static std::atomic<uint64_t> g_min_conn_requests{std::numeric_limits<uint64_t>::max()};
static std::atomic<uint64_t> g_max_conn_requests{0};

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
            {"content-length", std::to_string(kBenchmarkEchoBodySize)},
        });
    }();

    if (body_size == 0) {
        return kEncoded0;
    }
    if (body_size == kBenchmarkEchoBodySize) {
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
    static const auto kEncoded128 =
        std::make_shared<const std::string>(encodedEchoHeaders(kBenchmarkEchoBodySize));

    if (body_size == 0) {
        return kEncoded0;
    }
    if (body_size == kBenchmarkEchoBodySize) {
        return kEncoded128;
    }

    static thread_local std::unordered_map<size_t, std::shared_ptr<const std::string>> dynamic_cache;
    auto cached = dynamic_cache.find(body_size);
    if (cached != dynamic_cache.end()) {
        return cached->second;
    }

    auto encoded = std::make_shared<const std::string>(encodedEchoHeaders(body_size));
    dynamic_cache.emplace(body_size, encoded);
    return encoded;
}

void signalHandler(int) {
    g_running = false;
}

Task<void> handleActiveConn(Http2ConnContext& ctx) {
    if (g_debug_stats) {
        g_active_conn_handlers_started.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t local_request_count = 0;

    while (true) {
        auto streams = co_await ctx.getActiveStreams(64);
        if (!streams) {
            break;
        }

        for (auto& stream : *streams) {
            auto events = stream->takeEvents();
            if (g_debug_stats) {
                g_active_stream_visits.fetch_add(1, std::memory_order_relaxed);
            }
            if (g_debug_stats && hasHttp2StreamEvent(events, Http2StreamEvent::HeadersReady)) {
                g_headers_ready_events.fetch_add(1, std::memory_order_relaxed);
            }
            if (g_debug_stats && hasHttp2StreamEvent(events, Http2StreamEvent::DataArrived)) {
                g_data_arrived_events.fetch_add(1, std::memory_order_relaxed);
            }
            if (!hasHttp2StreamEvent(events, Http2StreamEvent::RequestComplete)) {
                if (g_debug_stats) {
                    g_active_stream_incomplete_visits.fetch_add(1, std::memory_order_relaxed);
                }
                continue;
            }

            if (g_debug_stats) {
                ++local_request_count;
                g_request_complete_events.fetch_add(1, std::memory_order_relaxed);
            }
            const size_t body_size = stream->request().bodySize();
            const size_t body_chunk_count = stream->request().bodyChunkCount();
            auto response_headers = sharedEncodedEchoHeaders(body_size);

            if (body_chunk_count == 1) {
                stream->sendEncodedHeadersAndData(
                    std::move(response_headers),
                    stream->request().takeSingleBodyChunk(),
                    true);
            } else if (body_chunk_count > 1) {
                stream->sendEncodedHeadersAndDataChunks(
                    std::string(*response_headers),
                    stream->request().takeBodyChunks(),
                    true);
            } else {
                stream->sendEncodedHeadersAndData(
                    std::move(response_headers),
                    std::string(),
                    true);
            }
        }
    }

    if (g_debug_stats) {
        if (local_request_count == 0) {
            g_active_conn_handlers_without_request.fetch_add(1, std::memory_order_relaxed);
        }
        if (local_request_count < 256) {
            g_conn_requests_lt_256.fetch_add(1, std::memory_order_relaxed);
        } else if (local_request_count < 512) {
            g_conn_requests_lt_512.fetch_add(1, std::memory_order_relaxed);
        } else if (local_request_count < 768) {
            g_conn_requests_lt_768.fetch_add(1, std::memory_order_relaxed);
        } else if (local_request_count < 1024) {
            g_conn_requests_lt_1024.fetch_add(1, std::memory_order_relaxed);
        } else if (local_request_count < 1280) {
            g_conn_requests_lt_1280.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_conn_requests_ge_1280.fetch_add(1, std::memory_order_relaxed);
        }

        auto min_seen = g_min_conn_requests.load(std::memory_order_relaxed);
        while (local_request_count < min_seen &&
               !g_min_conn_requests.compare_exchange_weak(
                   min_seen, local_request_count, std::memory_order_relaxed)) {
        }
        auto max_seen = g_max_conn_requests.load(std::memory_order_relaxed);
        while (local_request_count > max_seen &&
               !g_max_conn_requests.compare_exchange_weak(
                   max_seen, local_request_count, std::memory_order_relaxed)) {
        }

        if (g_running) {
            g_active_conn_handlers_finished_while_running.fetch_add(1, std::memory_order_relaxed);
        }
        g_active_conn_handlers_finished.fetch_add(1, std::memory_order_relaxed);
    }
    co_return;
}

Task<void> handleStream(Http2Stream::ptr stream) {
    while (true) {
        auto frame_result = co_await stream->getFrame();
        if (!frame_result || !frame_result.value()) {
            co_return;
        }
        auto frame = std::move(frame_result.value());
        if ((frame->isHeaders() || frame->isData()) && frame->isEndStream()) {
            break;
        }
    }

    auto& request = stream->request();
    if (request.method.empty()) {
        co_return;
    }

    const size_t body_size = request.bodySize();
    const size_t body_chunk_count = request.bodyChunkCount();
    auto response_headers = sharedEncodedEchoHeaders(body_size);

    if (body_chunk_count == 1) {
        stream->sendEncodedHeadersAndData(
            std::move(response_headers),
            request.takeSingleBodyChunk(),
            true);
    } else if (body_chunk_count > 1) {
        stream->sendEncodedHeadersAndDataChunks(
            std::string(*response_headers),
            request.takeBodyChunks(),
            true);
    } else {
        stream->sendEncodedHeadersAndData(
            std::move(response_headers),
            std::string(),
            true);
    }
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9443;
    int io_threads = 4;
    std::string cert_path = "../cert/test.crt";
    std::string key_path = "../cert/test.key";

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) io_threads = std::atoi(argv[2]);
    if (argc > 3) cert_path = argv[3];
    if (argc > 4) key_path = argv[4];
    if (const char* debug_log = std::getenv("GALAY_H2_DEBUG_LOG")) {
        g_debug_log = std::atoi(debug_log) != 0;
    }
    if (const char* debug_stats = std::getenv("GALAY_H2_DEBUG_STATS")) {
        g_debug_stats = std::atoi(debug_stats) != 0;
    }

    if (g_debug_log) {
    } else {
    }
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Server Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "Configured Compute Threads: 0\n";
    std::cout << "Cert: " << cert_path << "\n";
    std::cout << "Key:  " << key_path << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    try {
        H2Server server(H2ServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .certPath(cert_path)
            .keyPath(key_path)
            .ioSchedulerCount(static_cast<size_t>(io_threads))
            .computeSchedulerCount(0)
            .maxConcurrentStreams(1000)
            .initialWindowSize(65535)
            .flowControlTargetWindow(1u << 20)
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

        if (g_debug_stats) {
            std::cerr << "[debug-stats] active_started="
                      << g_active_conn_handlers_started.load(std::memory_order_relaxed)
                      << " active_finished="
                      << g_active_conn_handlers_finished.load(std::memory_order_relaxed)
                      << " active_finished_while_running="
                      << g_active_conn_handlers_finished_while_running.load(std::memory_order_relaxed)
                      << " active_without_request="
                      << g_active_conn_handlers_without_request.load(std::memory_order_relaxed)
                      << " active_stream_visits="
                      << g_active_stream_visits.load(std::memory_order_relaxed)
                      << " active_stream_incomplete_visits="
                      << g_active_stream_incomplete_visits.load(std::memory_order_relaxed)
                      << " headers_ready_events="
                      << g_headers_ready_events.load(std::memory_order_relaxed)
                      << " data_arrived_events="
                      << g_data_arrived_events.load(std::memory_order_relaxed)
                      << " request_complete_events="
                      << g_request_complete_events.load(std::memory_order_relaxed)
                      << " conn_req_lt_256="
                      << g_conn_requests_lt_256.load(std::memory_order_relaxed)
                      << " conn_req_lt_512="
                      << g_conn_requests_lt_512.load(std::memory_order_relaxed)
                      << " conn_req_lt_768="
                      << g_conn_requests_lt_768.load(std::memory_order_relaxed)
                      << " conn_req_lt_1024="
                      << g_conn_requests_lt_1024.load(std::memory_order_relaxed)
                      << " conn_req_lt_1280="
                      << g_conn_requests_lt_1280.load(std::memory_order_relaxed)
                      << " conn_req_ge_1280="
                      << g_conn_requests_ge_1280.load(std::memory_order_relaxed)
                      << " min_conn_requests="
                      << g_min_conn_requests.load(std::memory_order_relaxed)
                      << " max_conn_requests="
                      << g_max_conn_requests.load(std::memory_order_relaxed)
                      << "\n";
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

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
