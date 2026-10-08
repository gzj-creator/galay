/**
 * @file E13-H2EchoServer.cpp
 * @brief h2 (HTTP/2 over TLS) Echo 服务器示例
 */

#include <galay/cpp/galay-http2/server/http2_server.h>
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <cstdlib>

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_requests{0};

void signal_handler(int) {
    g_running = false;
}

Task<void> handle_stream(Http2Stream::ptr stream) {
    g_requests++;

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
    if (req.method.empty()) {
        co_return;
    }


    std::string body = req.body.empty() ? "Echo: (empty)" : ("Echo: " + req.coalesced_body());
    co_await stream->reply_header(
        Http2Headers()
            .status(200)
            .content_type("text/plain")
            .server("Galay-H2-Echo/1.0")
            .content_length(body.size()),
        body.empty());
    if (!body.empty()) {
        co_await stream->reply_data(body, true);
    }
    co_return;
}

int main(int argc, char* argv[]) {
    int port = 9443;
    std::string cert_path = "test/test.crt";
    std::string key_path = "test/test.key";

    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) cert_path = argv[2];
    if (argc > 3) key_path = argv[3];

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Echo Server Example\n";
    std::cout << "========================================\n";
    std::cout << "Server: https://0.0.0.0:" << port << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n";

    try {
        H2Server server(H2ServerBuilder<>()
            .host("0.0.0.0")
            .port(static_cast<uint16_t>(port))
            .cert_path(cert_path)
            .key_path(key_path)
            .io_scheduler_count(4)
            .max_concurrent_streams(100)
            .build_config());

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

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
