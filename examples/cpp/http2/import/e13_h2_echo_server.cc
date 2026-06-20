#include "common/example_common.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

import galay.http2;

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
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

    auto& req = stream->request();
    if (req.method.empty()) {
        co_return;
    }

    std::string body = req.body.empty() ? "Echo: (empty)" : ("Echo: " + req.coalescedBody());
    co_await stream->replyHeader(
        Http2Headers()
            .status(200)
            .contentType("text/plain")
            .server("Galay-H2-Import/1.0")
            .contentLength(body.size()),
        body.empty());
    if (!body.empty()) {
        co_await stream->replyData(body, true);
    }
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = galay::http::example::kDefaultH2EchoPort;
    std::string cert_path = "test/test.crt";
    std::string key_path = "test/test.key";

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        cert_path = argv[2];
    }
    if (argc > 3) {
        key_path = argv[3];
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        H2Server server(H2ServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .certPath(cert_path)
            .keyPath(key_path)
            .ioSchedulerCount(2)
            .maxConcurrentStreams(100)
            .build());
        std::cout << "Import h2 server: https://127.0.0.1:" << port << "\n";
        server.start(handleStream);

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        server.stop();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
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
