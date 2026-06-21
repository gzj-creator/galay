/**
 * @file T28-H2Server.cc
 * @brief H2 (HTTP/2 over TLS) 服务器测试程序
 */

#include <galay/cpp/galay-http2/server/http2_server.h>
#include <iostream>
#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>
#include <cstdlib>

using namespace galay::http2;
using namespace galay::kernel;

#ifdef GALAY_SSL_FEATURE_ENABLED

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_request_count{0};

void signalHandler(int) {
    g_running = false;
}

Task<void> handleStream(Http2Stream::ptr stream) {
    g_request_count++;

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


    std::string body = "Hello from H2 Test Server!\n";
    body += "Request #" + std::to_string(g_request_count.load()) + "\n";
    body += "Stream ID: " + std::to_string(stream->streamId()) + "\n";

    co_await stream->replyHeader(
        Http2Headers()
            .status(200)
            .contentType("text/plain")
            .server("Galay-H2-Test/1.0")
            .contentLength(body.size()),
        false);
    co_await stream->replyData(body, true);

    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9443;
    std::string cert_path = "test/test.crt";
    std::string key_path = "test/test.key";

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) cert_path = argv[2];
    if (argc > 3) key_path = argv[3];

    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Server Test\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "Cert: " << cert_path << "\n";
    std::cout << "Key:  " << key_path << "\n";
    std::cout << "Test command: ./test/T29-H2Client localhost " << port << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        H2Server server(H2ServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .certPath(cert_path)
            .keyPath(key_path)
            .ioSchedulerCount(4)
            .computeSchedulerCount(0)
            .maxConcurrentStreams(100)
            .initialWindowSize(65535)
            .enablePush(false)
            .build());

        server.start(handleStream);

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nTotal requests handled: " << g_request_count << "\n";
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
