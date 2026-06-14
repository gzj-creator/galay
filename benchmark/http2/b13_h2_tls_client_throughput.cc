/**
 * @file b13_h2.cc
 * @brief H2 (HTTP/2 over TLS) 客户端压测程序
 */

#include "kernel/kernel/runtime.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <algorithm>
#include <cstdlib>
#include <vector>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "http2/client/h2_client.h"
#endif

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::http2;
using namespace galay::kernel;

static const std::string kEchoPayload = "hello-h2-echo";

std::atomic<int> total_requests{0};
std::atomic<int> success_count{0};
std::atomic<int> fail_count{0};
std::atomic<int> connected_clients{0};
std::atomic<int> connect_failures{0};
std::atomic<int> active_clients{0};

struct ActiveClientGuard {
    ActiveClientGuard() { active_clients.fetch_add(1); }
    ~ActiveClientGuard() { active_clients.fetch_sub(1); }
};

struct BatchBarrier {
    void addOne() {
        remaining.fetch_add(1, std::memory_order_relaxed);
    }

    void completeOne() {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            done.notify();
        }
    }

    bool hasPending() const {
        return remaining.load(std::memory_order_acquire) > 0;
    }

    std::atomic<int> remaining{0};
    AsyncWaiter<void> done;
};

struct BatchBarrierGuard {
    explicit BatchBarrierGuard(std::shared_ptr<BatchBarrier> barrier)
        : m_barrier(std::move(barrier))
    {
    }

    ~BatchBarrierGuard() {
        if (m_barrier) {
            m_barrier->completeOne();
        }
    }

private:
    std::shared_ptr<BatchBarrier> m_barrier;
};

Task<void> handleResponse(Http2Stream::ptr stream, std::shared_ptr<BatchBarrier> barrier) {
    BatchBarrierGuard guard(std::move(barrier));

    bool finished = false;
    while (!finished) {
        auto batch_result = co_await stream->getFrames(16);
        if (!batch_result) {
            fail_count++;
            co_return;
        }
        auto frames = std::move(batch_result.value());
        bool stream_closed = false;
        for (auto& frame : frames) {
            if (!frame) {
                stream_closed = true;
                break;
            }
            if ((frame->isHeaders() || frame->isData()) && frame->isEndStream()) {
                finished = true;
                break;
            }
        }
        if (stream_closed) {
            fail_count++;
            co_return;
        }
    }

    auto& response = stream->response();
    if (response.status == 200 && response.body == kEchoPayload) {
        success_count++;
    } else {
        fail_count++;
    }
    co_return;
}

Task<void> runClient(int id,
                     const std::string& host,
                     uint16_t port,
                     int requests_per_client,
                     int streams_per_batch) {
    (void)id;
    ActiveClientGuard guard;

    H2Client client(H2ClientBuilder()
        .verifyPeer(false)
        .build());

    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        connect_failures++;
        fail_count += requests_per_client;
        co_return;
    }

    if (client.getALPNProtocol() != "h2") {
        fail_count += requests_per_client;
        co_await client.close();
        co_return;
    }

    connected_clients++;

    streams_per_batch = std::max(1, streams_per_batch);
    for (int sent = 0; sent < requests_per_client; ) {
        const int batch = std::min(streams_per_batch, requests_per_client - sent);
        auto barrier = std::make_shared<BatchBarrier>();

        for (int i = 0; i < batch; ++i) {
            auto stream = client.post("/echo", kEchoPayload, "text/plain");
            total_requests++;
            if (!stream) {
                fail_count++;
                continue;
            }
            barrier->addOne();
            co_await startDetachedTask(handleResponse(stream, barrier));
        }

        if (barrier->hasPending()) {
            co_await barrier->done.wait();
        }

        sent += batch;
    }

    co_await client.close();
    co_return;
}

void runBenchmark(const std::string& host,
                  uint16_t port,
                  int concurrent_clients,
                  int requests_per_client,
                  int streams_per_batch,
                  int max_wait_seconds,
                  int io_schedulers) {
    const int64_t expected_requests =
        static_cast<int64_t>(concurrent_clients) * requests_per_client;

    total_requests = 0;
    success_count = 0;
    fail_count = 0;
    connected_clients = 0;
    connect_failures = 0;
    active_clients = 0;

    std::cout << "\n========================================\n";
    std::cout << "测试配置:\n";
    std::cout << "  目标服务器: " << host << ":" << port << "\n";
    std::cout << "  并发客户端: " << concurrent_clients << "\n";
    std::cout << "  每客户端请求数: " << requests_per_client << "\n";
    std::cout << "  每连接并发流批次: " << streams_per_batch << "\n";
    std::cout << "  总请求数: " << expected_requests << "\n";
    std::cout << "  IO 调度器线程: " << io_schedulers << "\n";
    std::cout << "========================================\n\n";

    auto start_time = std::chrono::steady_clock::now();
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(io_schedulers).computeSchedulerCount(0).build();
    runtime.start();

    for (int i = 0; i < concurrent_clients; i++) {
        auto* scheduler = runtime.getNextIOScheduler();
        scheduleTask(scheduler, runClient(i, host, port, requests_per_client, streams_per_batch));
    }

    std::cout << "压测进行中";
    auto wait_begin = std::chrono::steady_clock::now();
    auto next_dot = wait_begin + std::chrono::seconds(1);
    while (true) {
        auto done = static_cast<int64_t>(success_count.load()) + fail_count.load();
        if (done >= expected_requests) {
            break;
        }

        if (active_clients.load() == 0) {
            auto decided_clients = connected_clients.load() + connect_failures.load();
            if (decided_clients >= concurrent_clients) {
                break;
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= next_dot) {
            std::cout << "." << std::flush;
            next_dot += std::chrono::seconds(1);
        }

        if (max_wait_seconds > 0 &&
            now - wait_begin >= std::chrono::seconds(max_wait_seconds)) {
            std::cerr << "\n[warn] wait timeout, active_clients=" << active_clients.load()
                      << ", done=" << done << "/" << expected_requests << "\n";
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "\n\n";

    runtime.stop();
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "========================================\n";
    std::cout << "测试结果:\n";
    std::cout << "========================================\n";
    std::cout << "连接成功: " << connected_clients << "/" << concurrent_clients << "\n";
    std::cout << "连接失败: " << connect_failures << "\n";
    std::cout << "请求成功: " << success_count << "\n";
    std::cout << "请求失败: " << fail_count << "\n";
    std::cout << "总耗时: " << duration.count() / 1000.0 << "s\n";
    if (duration.count() > 0) {
        double rps = (success_count.load() * 1000.0) / duration.count();
        std::cout << "请求吞吐: " << static_cast<int>(rps) << " req/s\n";
    }
    std::cout << "========================================\n\n";
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 9443;
    int concurrent_clients = 20;
    int requests_per_client = 20;
    int streams_per_batch = 16;
    int max_wait_seconds = 60;
    int io_schedulers = 2;

    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Client Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "用法: " << argv[0]
              << " [host] [port] [clients] [requests] [streams_per_batch] [max_wait] [io_threads]\n";
    std::cout << "示例: " << argv[0] << " 127.0.0.1 9443 400 400 16 180 4\n";
    std::cout << "========================================\n";

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 3) concurrent_clients = std::atoi(argv[3]);
    if (argc > 4) requests_per_client = std::atoi(argv[4]);
    if (argc > 5) streams_per_batch = std::max(1, std::atoi(argv[5]));
    if (argc > 6) max_wait_seconds = std::atoi(argv[6]);
    if (argc > 7) io_schedulers = std::max(1, std::atoi(argv[7]));

    runBenchmark(host, port, concurrent_clients, requests_per_client, streams_per_batch,
                 max_wait_seconds, io_schedulers);
    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
