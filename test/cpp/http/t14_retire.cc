#include <galay/cpp/galay-http2/server/http2_server.h>
#include <galay/cpp/galay-http2/client/h2c_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<bool> g_client_done{false};
static std::atomic<int> g_successful_rounds{0};
static std::atomic<int> g_active_deliveries{0};

Task<void> active_conn_handler(Http2ConnContext& ctx) {
    while (true) {
        auto streams = co_await ctx.get_active_streams(16);
        if (!streams) {
            break;
        }

        for (auto& stream : *streams) {
            g_active_deliveries.fetch_add(1, std::memory_order_relaxed);
            auto events = stream->take_events();
            if (!has_http2_stream_event(events, Http2StreamEvent::RequestComplete)) {
                continue;
            }

            const size_t body_size = stream->request().body_size();
            const size_t body_chunk_count = stream->request().body_chunk_count();
            stream->send_headers(
                Http2Headers().status(200).content_type("text/plain").content_length(body_size),
                body_size == 0, true);
            if (body_chunk_count == 1) {
                auto body = stream->request().take_single_body_chunk();
                stream->send_data(std::move(body), true);
            } else if (body_chunk_count > 1) {
                auto body_chunks = stream->request().take_body_chunks();
                stream->send_data_chunks(std::move(body_chunks), true);
            }
        }
    }

    co_return;
}

Task<void> run_client(uint16_t port) {
    H2cClient<> client(H2cClientBuilder().build());

    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result) {
        std::cerr << "[T44] client connect failed: " << connect_result.error().message() << "\n";
        g_client_done = true;
        co_return;
    }

    auto upgrade_result = co_await client.upgrade("/retire");
    if (!upgrade_result) {
        std::cerr << "[T44] client upgrade failed: " << upgrade_result.error().to_string() << "\n";
        g_client_done = true;
        co_return;
    }

    for (int i = 0; i < 3; ++i) {
        const std::string body = "round-" + std::to_string(i);
        auto stream = client.post("/retire", body, "text/plain");
        if (!stream) {
            std::cerr << "[T44] post returned null stream at round " << i << "\n";
            g_client_done = true;
            co_return;
        }

        auto response_done = co_await stream->wait_response_complete();
        if (!response_done) {
            std::cerr << "[T44] waitResponseComplete failed at round " << i << "\n";
            g_client_done = true;
            co_return;
        }

        if (stream->response().status != 200 || stream->response().body != body) {
            std::cerr << "[T44] unexpected response at round " << i
                      << " status=" << stream->response().status
                      << " body=" << stream->response().body << "\n";
            g_client_done = true;
            co_return;
        }

        g_successful_rounds.fetch_add(1, std::memory_order_relaxed);
    }

    auto shutdown_result = co_await client.shutdown();
    if (!shutdown_result) {
        std::cerr << "[T44] client shutdown failed\n";
        g_client_done = true;
        co_return;
    }

    g_client_done = true;
    co_return;
}

int main() {

    const uint16_t port = static_cast<uint16_t>(21000 + (::getpid() % 10000));

    H2cServer server(H2cServerBuilder<>()
        .host("127.0.0.1")
        .port(port)
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .max_concurrent_streams(2)
        .active_conn_handler(active_conn_handler)
        .build_config());

    if (const auto started = server.start(); !started) {

        std::cerr << "Server startup failed: " << started.error().message << '\n';

    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    runtime.start();
    auto join = runtime.spawn_io(run_client(port));
    if (!join) {
        std::cerr << "[T44] runtime.spawn failed: " << join.error().message() << "\n";
        runtime.stop();
        server.stop();
        return 1;
    }

    for (int i = 0; i < 120; ++i) {
        if (g_client_done.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (g_client_done.load(std::memory_order_acquire)) {
        auto join_result = join->join();
        if (!join_result) {
            std::cerr << "[T44] client join failed: " << join_result.error().message() << "\n";
        }
    }

    runtime.stop();
    server.stop();

    if (!g_client_done.load(std::memory_order_acquire)) {
        std::cerr << "[T44] client timed out\n";
        return 1;
    }

    if (g_successful_rounds.load(std::memory_order_acquire) != 3) {
        std::cerr << "[T44] expected 3 successful sequential requests, got "
                  << g_successful_rounds.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (g_active_deliveries.load(std::memory_order_acquire) != 3) {
        std::cerr << "[T44] expected 3 active stream deliveries, got "
                  << g_active_deliveries.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T44-H2ActiveConnRetire PASS\n";
    return 0;
}
