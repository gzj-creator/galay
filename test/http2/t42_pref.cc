#include "http2/server/http2_server.h"
#include "http2/client/h2c_client.h"
#include "kernel/kernel/runtime.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<int> g_stream_handler_calls{0};
static std::atomic<int> g_active_handler_calls{0};
static std::atomic<int> g_active_deliveries{0};
static std::atomic<uint32_t> g_seen_events{0};
static std::atomic<bool> g_client_done{false};
static std::atomic<bool> g_client_ok{false};

Task<void> legacyStreamHandler(Http2Stream::ptr) {
    g_stream_handler_calls.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

Task<void> activeConnHandler(Http2ConnContext& ctx) {
    g_active_handler_calls.fetch_add(1, std::memory_order_relaxed);

    while (true) {
        auto streams = co_await ctx.getActiveStreams(16);
        if (!streams) {
            break;
        }

        for (auto& stream : *streams) {
            g_active_deliveries.fetch_add(1, std::memory_order_relaxed);
            auto events = stream->takeEvents();
            g_seen_events.fetch_or(static_cast<uint32_t>(events), std::memory_order_relaxed);

            if (!hasHttp2StreamEvent(events, Http2StreamEvent::RequestComplete)) {
                continue;
            }

            const size_t body_size = stream->request().bodySize();
            const size_t body_chunk_count = stream->request().bodyChunkCount();
            stream->sendHeaders(
                Http2Headers().status(200).contentType("text/plain").contentLength(body_size),
                body_size == 0, true);
            if (body_chunk_count == 1) {
                auto body = stream->request().takeSingleBodyChunk();
                stream->sendData(std::move(body), true);
            } else if (body_chunk_count > 1) {
                auto body_chunks = stream->request().takeBodyChunks();
                stream->sendDataChunks(std::move(body_chunks), true);
            }
        }
    }

    co_return;
}

Task<void> runClient(uint16_t port) {
    H2cClient client(H2cClientBuilder().build());

    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result) {
        std::cerr << "[T42] client connect failed: " << connect_result.error().message() << "\n";
        g_client_done = true;
        co_return;
    }

    auto upgrade_result = co_await client.upgrade("/active");
    if (!upgrade_result) {
        std::cerr << "[T42] client upgrade failed: " << upgrade_result.error().toString() << "\n";
        g_client_done = true;
        co_return;
    }

    auto stream = client.post("/active", "ping", "text/plain");
    if (!stream) {
        std::cerr << "[T42] client post returned null stream\n";
        g_client_done = true;
        co_return;
    }

    auto response_done = co_await stream->waitResponseComplete();
    if (response_done &&
        stream->response().status == 200 &&
        stream->response().body == "ping") {
        g_client_ok = true;
    }

    auto shutdown_result = co_await client.shutdown();
    if (!shutdown_result) {
        std::cerr << "[T42] client shutdown failed\n";
        g_client_done = true;
        co_return;
    }
    g_client_done = true;
    co_return;
}

int main() {

    const uint16_t port = static_cast<uint16_t>(20000 + (::getpid() % 10000));

    H2cServer server(H2cServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .streamHandler(legacyStreamHandler)
        .activeConnHandler(activeConnHandler)
        .build());

    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();
    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "[T42] missing IO scheduler\n";
        server.stop();
        return 1;
    }

    scheduleTask(scheduler, runClient(port));

    for (int i = 0; i < 100; ++i) {
        if (g_client_done.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    runtime.stop();
    server.stop();

    if (!g_client_done.load(std::memory_order_acquire)) {
        std::cerr << "[T42] client timed out\n";
        return 1;
    }
    if (!g_client_ok.load(std::memory_order_acquire)) {
        std::cerr << "[T42] client did not receive expected response\n";
        return 1;
    }
    if (g_active_handler_calls.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T42] active handler should run exactly once per connection\n";
        return 1;
    }
    if (g_stream_handler_calls.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T42] legacy stream handler must be bypassed when active handler exists\n";
        return 1;
    }
    if (g_active_deliveries.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T42] expected exactly one active stream delivery, got "
                  << g_active_deliveries.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    const auto seen = static_cast<Http2StreamEvent>(g_seen_events.load(std::memory_order_acquire));
    if (!hasHttp2StreamEvent(seen, Http2StreamEvent::HeadersReady) ||
        !hasHttp2StreamEvent(seen, Http2StreamEvent::DataArrived) ||
        !hasHttp2StreamEvent(seen, Http2StreamEvent::RequestComplete)) {
        std::cerr << "[T42] active handler did not observe expected request events\n";
        return 1;
    }

    std::cout << "T42-H2ActiveConnPreferred PASS\n";
    return 0;
}
