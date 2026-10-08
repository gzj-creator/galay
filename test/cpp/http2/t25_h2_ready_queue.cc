/**
 * @file t90_h2_ready_queue.cc
 * @brief HTTP/2 active stream ready queue contract tests
 */

#include <sstream>

#define private public
#include <galay/cpp/galay-http2/kernel/stream_manager.h>
#undef private
#include <galay/cpp/galay-http2/client/h2c_client.h>
#include <galay/cpp/galay-http2/server/http2_server.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace galay::http2;
using namespace galay::kernel;

namespace {

std::atomic<bool> g_done{false};
std::atomic<bool> g_ok{false};
std::atomic<int> g_deliveries{0};
std::atomic<int> g_empty_batches{0};

Task<void> active_handler(Http2ConnContext& ctx)
{
    while (true) {
        auto streams = co_await ctx.get_active_streams(4);
        if (!streams) {
            break;
        }
        if (streams->empty()) {
            g_empty_batches.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        for (auto& stream : *streams) {
            auto events = stream->take_events();
            if (!has_http2_stream_event(events, Http2StreamEvent::RequestComplete)) {
                continue;
            }
            g_deliveries.fetch_add(1, std::memory_order_relaxed);
            stream->send_headers(
                Http2Headers().status(200).content_type("text/plain").content_length(0),
                true,
                true);
        }
    }
    co_return;
}

Task<void> run_client(uint16_t port)
{
    H2cClient<> client(H2cClientBuilder().build());
    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result) {
        std::cerr << "[T90] connect failed: " << connect_result.error().message() << "\n";
        g_done = true;
        co_return;
    }
    auto upgrade_result = co_await client.upgrade("/ready");
    if (!upgrade_result) {
        std::cerr << "[T90] upgrade failed: " << upgrade_result.error().to_string() << "\n";
        g_done = true;
        co_return;
    }

    std::vector<Http2Stream::ptr> streams;
    streams.reserve(8);
    for (int i = 0; i < 8; ++i) {
        streams.push_back(client.get("/ready"));
    }
    for (auto& stream : streams) {
        auto done = co_await stream->wait_response_complete();
        if (!done || stream->response().status != 200) {
            std::cerr << "[T90] stream did not complete with 200\n";
            g_done = true;
            co_return;
        }
    }

    co_await client.shutdown();
    g_ok = true;
    g_done = true;
    co_return;
}

} // namespace

int main()
{
    Http2StreamPool pool;
    auto first = pool.acquire(1);
    auto second = pool.acquire(3);

    Http2ActiveStreamBatch batch;
    batch.mark(first, Http2StreamEvent::HeadersReady);
    batch.mark(second, Http2StreamEvent::RequestComplete);
    batch.mark(first, Http2StreamEvent::DataArrived);

    auto ready = batch.take_ready();
    assert(ready.size() == 2);
    assert(ready[0] == first);
    assert(ready[1] == second);

    auto first_events = first->take_events();
    assert(has_http2_stream_event(first_events, Http2StreamEvent::HeadersReady));
    assert(has_http2_stream_event(first_events, Http2StreamEvent::DataArrived));
    assert(!has_http2_stream_event(first_events, Http2StreamEvent::RequestComplete));

    auto second_events = second->take_events();
    assert(has_http2_stream_event(second_events, Http2StreamEvent::RequestComplete));

    auto duplicate = batch.take_ready();
    assert(duplicate.empty());

    batch.mark(first, Http2StreamEvent::RequestComplete);
    auto ready_again = batch.take_ready();
    assert(ready_again.size() == 1);
    assert(ready_again[0] == first);
    auto first_again_events = first->take_events();
    assert(has_http2_stream_event(first_again_events, Http2StreamEvent::RequestComplete));

    const uint16_t port = static_cast<uint16_t>(24000 + (::getpid() % 10000));
    H2cServer server(H2cServerBuilder<>()
        .host("127.0.0.1")
        .port(port)
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .active_conn_handler(active_handler)
        .build_config());
    if (const auto started = server.start(); !started) {
        std::cerr << "Server startup failed: " << started.error().message << '\n';
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    runtime.start();
    auto* scheduler = runtime.get_next_io_scheduler();
    assert(scheduler != nullptr);
    schedule_task(scheduler, run_client(port));

    for (int i = 0; i < 100; ++i) {
        if (g_done.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    runtime.stop();
    server.stop();

    assert(g_done.load(std::memory_order_acquire));
    assert(g_ok.load(std::memory_order_acquire));
    assert(g_deliveries.load(std::memory_order_acquire) == 8);
    assert(g_empty_batches.load(std::memory_order_acquire) == 0);

    std::cout << "t90_h2_ready_queue PASS\n";
    return 0;
}
