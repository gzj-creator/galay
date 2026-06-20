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

void signalHandler(int) {
    g_running = false;
}

Task<void> handleStream(Http2Stream::ptr stream) {
    g_requests++;

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
    auto& req = stream->request();
    if (req.method.empty()) {
        co_return;
    }


    std::string body = req.body.empty() ? "Echo: (empty)" : ("Echo: " + req.coalescedBody());
    co_await stream->replyHeader(
        Http2Headers()
            .status(200)
            .contentType("text/plain")
            .server("Galay-H2-Echo/1.0")
            .contentLength(body.size()),
        body.empty());
    if (!body.empty()) {
        co_await stream->replyData(body, true);
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

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Echo Server Example\n";
    std::cout << "========================================\n";
    std::cout << "Server: https://0.0.0.0:" << port << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n";

    try {
        H2Server server(H2ServerBuilder()
            .host("0.0.0.0")
            .port(static_cast<uint16_t>(port))
            .certPath(cert_path)
            .keyPath(key_path)
            .ioSchedulerCount(4)
            .maxConcurrentStreams(100)
            .build());

        server.start(handleStream);

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
