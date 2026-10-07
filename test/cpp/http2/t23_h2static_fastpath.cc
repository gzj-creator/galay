/**
 * @file t88_h2static_fastpath.cc
 * @brief HTTP/2 静态空响应 fast path 行为测试
 */

#include <galay/cpp/galay-http2/client/h2c_client.h>
#include <galay/cpp/galay-http2/server/http2_server.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace galay::http2;
using namespace galay::kernel;

namespace {

const std::string kSmallBody(1024, 's');
std::atomic<int> g_active_deliveries{0};
std::atomic<bool> g_done{false};
std::atomic<bool> g_ok{false};

Task<void> fallback_active_handler(Http2ConnContext& ctx)
{
    while (true) {
        auto streams = co_await ctx.get_active_streams(16);
        if (!streams) {
            break;
        }

        for (auto& stream : *streams) {
            auto events = stream->take_events();
            if (!has_http2_stream_event(events, Http2StreamEvent::RequestComplete)) {
                continue;
            }
            g_active_deliveries.fetch_add(1, std::memory_order_relaxed);
            stream->send_headers(
                Http2Headers().status(500).content_type("text/plain").content_length(0),
                true,
                true);
        }
    }
    co_return;
}

Http2Stream::ptr send_head(H2cClient<>& client, uint16_t port, const std::string& path)
{
    auto* conn = client.get_conn();
    if (conn == nullptr || conn->stream_manager() == nullptr) {
        return nullptr;
    }

    auto stream = conn->stream_manager()->allocate_stream();
    const std::string authority = "127.0.0.1:" + std::to_string(port);
    stream->send_headers({
        {":method", "HEAD"},
        {":scheme", "http"},
        {":authority", authority},
        {":path", path},
    }, true, true);
    return stream;
}

bool is_empty_ok_response(const Http2Stream::ptr& stream)
{
    return stream &&
           stream->response().status == 200 &&
           stream->response().body.empty() &&
           stream->is_response_completed();
}

std::string response_header(const Http2Stream::ptr& stream, const std::string& name)
{
    if (!stream) {
        return "";
    }
    for (const auto& header : stream->response().headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return "";
}

bool is_small_ok_response(const Http2Stream::ptr& stream)
{
    return stream &&
           stream->response().status == 200 &&
           stream->response().body == kSmallBody &&
           response_header(stream, "content-length") == std::to_string(kSmallBody.size()) &&
           stream->is_response_completed();
}

bool is_small_head_ok_response(const Http2Stream::ptr& stream)
{
    return stream &&
           stream->response().status == 200 &&
           stream->response().body.empty() &&
           response_header(stream, "content-length") == std::to_string(kSmallBody.size()) &&
           stream->is_response_completed();
}

Task<void> run_client(uint16_t port)
{
    H2cClient<> client(H2cClientBuilder().build());

    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result) {
        std::cerr << "[T88] connect failed: " << connect_result.error().message() << "\n";
        g_done = true;
        co_return;
    }

    auto upgrade_result = co_await client.upgrade("/echo");
    if (!upgrade_result) {
        std::cerr << "[T88] upgrade failed: " << upgrade_result.error().to_string() << "\n";
        g_done = true;
        co_return;
    }

    auto get_stream = client.get("/echo");
    if (!get_stream) {
        std::cerr << "[T88] GET stream allocation failed\n";
        g_done = true;
        co_return;
    }
    auto get_done = co_await get_stream->wait_response_complete();
    if (!get_done || !is_empty_ok_response(get_stream)) {
        std::cerr << "[T88] GET did not receive static empty 200 response, status="
                  << (get_stream ? get_stream->response().status : 0) << "\n";
        g_done = true;
        co_return;
    }

    auto head_stream = send_head(client, port, "/echo");
    if (!head_stream) {
        std::cerr << "[T88] HEAD stream allocation failed\n";
        g_done = true;
        co_return;
    }
    auto head_done = co_await head_stream->wait_response_complete();
    if (!head_done || !is_empty_ok_response(head_stream)) {
        std::cerr << "[T88] HEAD did not receive static empty 200 response, status="
                  << (head_stream ? head_stream->response().status : 0) << "\n";
        g_done = true;
        co_return;
    }

    auto small_get_stream = client.get("/small");
    if (!small_get_stream) {
        std::cerr << "[T88] small GET stream allocation failed\n";
        g_done = true;
        co_return;
    }
    auto small_get_done = co_await small_get_stream->wait_response_complete();
    if (!small_get_done || !is_small_ok_response(small_get_stream)) {
        std::cerr << "[T88] GET /small did not receive 1KB static response, status="
                  << (small_get_stream ? small_get_stream->response().status : 0)
                  << " body_size="
                  << (small_get_stream ? small_get_stream->response().body.size() : 0)
                  << " content-length=" << response_header(small_get_stream, "content-length")
                  << "\n";
        g_done = true;
        co_return;
    }

    auto small_head_stream = send_head(client, port, "/small");
    if (!small_head_stream) {
        std::cerr << "[T88] small HEAD stream allocation failed\n";
        g_done = true;
        co_return;
    }
    auto small_head_done = co_await small_head_stream->wait_response_complete();
    if (!small_head_done || !is_small_head_ok_response(small_head_stream)) {
        std::cerr << "[T88] HEAD /small did not receive headers-only 1KB metadata, status="
                  << (small_head_stream ? small_head_stream->response().status : 0)
                  << " body_size="
                  << (small_head_stream ? small_head_stream->response().body.size() : 0)
                  << " content-length=" << response_header(small_head_stream, "content-length")
                  << "\n";
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
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .static_response("/echo", H2StaticResponse{
            .status = 200,
            .content_type = "text/plain",
            .body = "",
        })
        .static_response("/small", H2StaticResponse{
            .status = 200,
            .content_type = "text/plain",
            .body = kSmallBody,
        })
        .active_conn_handler(fallback_active_handler)
        .build());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    runtime.start();
    auto* scheduler = runtime.get_next_io_scheduler();
    if (!scheduler) {
        std::cerr << "[T88] missing IO scheduler\n";
        server.stop();
        return 1;
    }
    schedule_task(scheduler, run_client(port));

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
