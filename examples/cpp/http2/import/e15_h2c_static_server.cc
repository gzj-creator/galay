#include "common/example_common.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

import galay.http2;

using namespace galay::http2;
using namespace galay::kernel;

namespace {

std::atomic<bool> g_running{true};

void signalHandler(int)
{
    g_running = false;
}

Task<void> fallbackActiveHandler(Http2ConnContext& ctx)
{
    while (true) {
        auto streams = co_await ctx.getActiveStreams(16);
        if (!streams) {
            break;
        }
        for (auto& stream : *streams) {
            auto events = stream->takeEvents();
            if (!hasHttp2StreamEvent(events, Http2StreamEvent::RequestComplete)) {
                continue;
            }
            stream->sendHeaders(
                Http2Headers().status(404).contentType("text/plain").contentLength(0),
                true,
                true);
        }
    }
    co_return;
}

void ensureExampleFiles(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    const auto index = root / "index.txt";
    if (!std::filesystem::exists(index)) {
        std::ofstream(index) << "hello from import h2c static server\n";
    }
}

} // namespace

int main(int argc, char* argv[])
{
    uint16_t port = galay::http::example::kDefaultH2cEchoPort;
    std::filesystem::path static_root = "examples/cpp/http2/public";
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        static_root = argv[2];
    }

    ensureExampleFiles(static_root);
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    H2cServer server(H2cServerBuilder()
        .host("0.0.0.0")
        .port(port)
        .ioSchedulerCount(2)
        .computeSchedulerCount(0)
        .staticResponse("/healthz", H2StaticResponse{
            .status = 200,
            .content_type = "text/plain",
            .body = "ok",
        })
        .staticFiles("/files", H2StaticFileConfig{.root = static_root})
        .activeConnHandler(fallbackActiveHandler)
        .build());

    std::cout << "Import h2c static server: http://127.0.0.1:" << port << "\n";
    std::cout << "Static root: " << static_root << "\n";
    server.start();
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.stop();
    return 0;
}
