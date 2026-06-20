/**
 * @file t88_h2static_fastpath.cc
 * @brief HTTP/2 静态空响应 fast path 行为测试
 */

#include "galay-http2/client/h2c_client.h"
#include "galay-http2/server/http2_server.h"
#include "galay-kernel/core/runtime.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace galay::http2;
using namespace galay::kernel;

namespace {

std::atomic<int> g_active_deliveries{0};
std::atomic<bool> g_done{false};
std::atomic<bool> g_ok{false};

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
            g_active_deliveries.fetch_add(1, std::memory_order_relaxed);
            stream->sendHeaders(
                Http2Headers().status(500).contentType("text/plain").contentLength(0),
                true,
                true);
        }
    }
    co_return;
}

Http2Stream::ptr sendHead(H2cClient& client, uint16_t port, const std::string& path)
{
    auto* conn = client.getConn();
    if (conn == nullptr || conn->streamManager() == nullptr) {
        return nullptr;
    }

    auto stream = conn->streamManager()->allocateStream();
    const std::string authority = "127.0.0.1:" + std::to_string(port);
    stream->sendHeaders({
        {":method", "HEAD"},
        {":scheme", "http"},
        {":authority", authority},
        {":path", path},
    }, true, true);
    return stream;
}

bool isEmptyOkResponse(const Http2Stream::ptr& stream)
{
    return stream &&
           stream->response().status == 200 &&
           stream->response().body.empty() &&
           stream->isResponseCompleted();
}

Task<void> runClient(uint16_t port)
{
    H2cClient client(H2cClientBuilder().build());

    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result) {
        std::cerr << "[T88] connect failed: " << connect_result.error().message() << "\n";
        g_done = true;
        co_return;
    }

    auto upgrade_result = co_await client.upgrade("/echo");
    if (!upgrade_result) {
        std::cerr << "[T88] upgrade failed: " << upgrade_result.error().toString() << "\n";
        g_done = true;
        co_return;
    }

    auto get_stream = client.get("/echo");
    if (!get_stream) {
        std::cerr << "[T88] GET stream allocation failed\n";
        g_done = true;
        co_return;
    }
    auto get_done = co_await get_stream->waitResponseComplete();
    if (!get_done || !isEmptyOkResponse(get_stream)) {
        std::cerr << "[T88] GET did not receive static empty 200 response, status="
                  << (get_stream ? get_stream->response().status : 0) << "\n";
        g_done = true;
        co_return;
    }

    auto head_stream = sendHead(client, port, "/echo");
    if (!head_stream) {
        std::cerr << "[T88] HEAD stream allocation failed\n";
        g_done = true;
        co_return;
    }
    auto head_done = co_await head_stream->waitResponseComplete();
    if (!head_done || !isEmptyOkResponse(head_stream)) {
        std::cerr << "[T88] HEAD did not receive static empty 200 response, status="
                  << (head_stream ? head_stream->response().status : 0) << "\n";
        g_done = true;
        co_return;
    }

    auto shutdown_result = co_await client.shutdown();
    if (!shutdown_result) {
        std::cerr << "[T88] shutdown failed\n";
        g_done = true;
        co_return;
    }

    g_ok = true;
    g_done = true;
    co_return;
}

} // namespace

int main()
{
    const uint16_t port = static_cast<uint16_t>(22000 + (::getpid() % 10000));

    H2cServer server(H2cServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .staticResponse("/echo", H2StaticResponse{
            .status = 200,
            .content_type = "text/plain",
            .body = "",
        })
        .activeConnHandler(fallbackActiveHandler)
        .build());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();
    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "[T88] missing IO scheduler\n";
        server.stop();
        return 1;
    }
    scheduleTask(scheduler, runClient(port));

    for (int i = 0; i < 100; ++i) {
        if (g_done.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    runtime.stop();
    server.stop();

    if (!g_done.load(std::memory_order_acquire)) {
        std::cerr << "[T88] client timed out\n";
        return 1;
    }
    if (!g_ok.load(std::memory_order_acquire)) {
        return 1;
    }
    if (g_active_deliveries.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T88] static fast path must bypass active handler, deliveries="
                  << g_active_deliveries.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "t88_h2static_fastpath PASS\n";
    return 0;
}
