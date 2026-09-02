#include "common/example_common.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

import galay.http2;

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
    co_await stream->replyHeader(
        Http2Headers().status(200).contentType("text/plain")
            .server("Galay-H2c-Import/1.0").contentLength(req.body.size()),
        req.body.empty());
    if (!req.body.empty()) {
        co_await stream->replyData(req.takeSingleBodyChunk(), true);
    }
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = galay::http::example::kDefaultH2cEchoPort;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        H2cServer server(H2cServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .ioSchedulerCount(2)
            .enablePush(false)
            .build());
        std::cout << "Import h2c server: http://127.0.0.1:" << port << "\n";
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
