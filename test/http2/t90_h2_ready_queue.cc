/**
 * @file t90_h2_ready_queue.cc
 * @brief HTTP/2 active stream ready queue contract tests
 */

#include <sstream>

#define private public
#include "galay-http2/kernel/stream_manager.h"
#undef private
#include "galay-http2/client/h2c_client.h"
#include "galay-http2/server/http2_server.h"
#include "galay-kernel/core/runtime.h"

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

Task<void> activeHandler(Http2ConnContext& ctx)
{
    while (true) {
        auto streams = co_await ctx.getActiveStreams(4);
        if (!streams) {
            break;
        }
        if (streams->empty()) {
            g_empty_batches.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        for (auto& stream : *streams) {
            auto events = stream->takeEvents();
            if (!hasHttp2StreamEvent(events, Http2StreamEvent::RequestComplete)) {
                continue;
            }
            g_deliveries.fetch_add(1, std::memory_order_relaxed);
            stream->sendHeaders(
                Http2Headers().status(200).contentType("text/plain").contentLength(0),
                true,
                true);
        }
    }
    co_return;
}

Task<void> runClient(uint16_t port)
{
    H2cClient client(H2cClientBuilder().build());
    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result) {
        std::cerr << "[T90] connect failed: " << connect_result.error().message() << "\n";
        g_done = true;
        co_return;
    }
    auto upgrade_result = co_await client.upgrade("/ready");
    if (!upgrade_result) {
        std::cerr << "[T90] upgrade failed: " << upgrade_result.error().toString() << "\n";
        g_done = true;
        co_return;
    }

    std::vector<Http2Stream::ptr> streams;
    streams.reserve(8);
    for (int i = 0; i < 8; ++i) {
        streams.push_back(client.get("/ready"));
    }
    for (auto& stream : streams) {
        auto done = co_await stream->waitResponseComplete();
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

    auto ready = batch.takeReady();
    assert(ready.size() == 2);
    assert(ready[0] == first);
    assert(ready[1] == second);

    auto first_events = first->takeEvents();
    assert(hasHttp2StreamEvent(first_events, Http2StreamEvent::HeadersReady));
    assert(hasHttp2StreamEvent(first_events, Http2StreamEvent::DataArrived));
    assert(!hasHttp2StreamEvent(first_events, Http2StreamEvent::RequestComplete));

    auto second_events = second->takeEvents();
    assert(hasHttp2StreamEvent(second_events, Http2StreamEvent::RequestComplete));

    auto duplicate = batch.takeReady();
    assert(duplicate.empty());

    batch.mark(first, Http2StreamEvent::RequestComplete);
    auto ready_again = batch.takeReady();
    assert(ready_again.size() == 1);
    assert(ready_again[0] == first);
    auto first_again_events = first->takeEvents();
    assert(hasHttp2StreamEvent(first_again_events, Http2StreamEvent::RequestComplete));

    const uint16_t port = static_cast<uint16_t>(24000 + (::getpid() % 10000));
    H2cServer server(H2cServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .activeConnHandler(activeHandler)
        .build());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();
    auto* scheduler = runtime.getNextIOScheduler();
    assert(scheduler != nullptr);
    scheduleTask(scheduler, runClient(port));

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
