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

void signal_handler(int) {
    g_running = false;
}

Task<void> handle_stream(Http2Stream::ptr stream) {
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
    co_await stream->reply_header(
        Http2Headers().status(200).content_type("text/plain")
            .server("Galay-H2c-Import/1.0").content_length(req.body.size()),
        req.body.empty());
    if (!req.body.empty()) {
        co_await stream->reply_data(req.take_single_body_chunk(), true);
    }
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = galay::http::example::kDefaultH2cEchoPort;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        H2cServer server(H2cServerBuilder<>()
            .host("0.0.0.0")
            .port(port)
            .io_scheduler_count(2)
            .enable_push(false)
            .build_config());
        std::cout << "Import h2c server: http://127.0.0.1:" << port << "\n";
        server.start(handle_stream);
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
