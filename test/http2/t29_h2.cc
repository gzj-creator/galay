/**
 * @file T28-H2Client.cc
 * @brief H2 (HTTP/2 over TLS) 客户端测试程序
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include <galay/cpp/galay-http2/client/h2_client.h>
#endif

using namespace galay::kernel;

#ifdef GALAY_SSL_FEATURE_ENABLED
using namespace galay::http2;

static std::atomic<int> g_success{0};
static std::atomic<int> g_fail{0};

Task<void> runClient(const std::string& host, uint16_t port, int num_requests) {
    H2Client client(H2ClientBuilder()
        .verifyPeer(false)
        .build());

    auto connect_task_result = co_await client.connect(host, port);
    if (!connect_task_result) {
        std::cerr << "[connect-fail] " << connect_task_result.error().message() << "\n";
        g_fail += num_requests;
        co_return;
    }

    auto connect_result = std::move(connect_task_result.value());
    if (!connect_result || !connect_result.value()) {
        if (!connect_result) {
            std::cerr << "[connect-fail] " << static_cast<int>(connect_result.error()) << "\n";
        } else {
            std::cerr << "[connect-fail] false\n";
        }
        g_fail += num_requests;
        co_return;
    }

    std::string alpn = client.getALPNProtocol();
    std::cout << "Negotiated ALPN: " << (alpn.empty() ? "(empty)" : alpn) << "\n";
    if (alpn != "h2") {
        std::cerr << "[alpn-fail] expected h2, got " << alpn << "\n";
        g_fail += num_requests;
        co_await client.close();
        co_return;
    }

    for (int i = 0; i < num_requests; i++) {
        auto stream = client.get("/h2/test");
        if (!stream) {
            g_fail++;
            continue;
        }

        bool finished = false;
        while (!finished) {
            auto frame_result = co_await stream->getFrame();
            if (!frame_result || !frame_result.value()) {
                break;
            }
            auto frame = std::move(frame_result.value());
            if ((frame->isHeaders() || frame->isData()) && frame->isEndStream()) {
                finished = true;
            }
        }

        if (finished) {
            g_success++;
        } else {
            g_fail++;
        }
    }

    co_await client.close();
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 9443;
    int requests = 20;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 3) requests = std::atoi(argv[3]);

    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Client Test\n";
    std::cout << "========================================\n";
    std::cout << "Target: " << host << ":" << port << "\n";
    std::cout << "Requests: " << requests << "\n";
    std::cout << "========================================\n\n";

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "No IO scheduler available\n";
        return 1;
    }

    auto join = runtime.spawn(runClient(host, port, requests));
    if (!join) {
        std::cerr << "Failed to spawn client coroutine: " << join.error().message() << "\n";
        runtime.stop();
        return 1;
    }
    auto join_result = join->join();
    if (!join_result) {
        std::cerr << "Client coroutine join failed: " << join_result.error().message() << "\n";
        runtime.stop();
        return 1;
    }

    runtime.stop();

    std::cout << "\n========================================\n";
    std::cout << "Test Results\n";
    std::cout << "========================================\n";
    std::cout << "Success: " << g_success.load() << "\n";
    std::cout << "Failed:  " << g_fail.load() << "\n";
    std::cout << "Total:   " << (g_success.load() + g_fail.load()) << "\n";
    std::cout << "========================================\n";

    return g_fail.load() == 0 ? 0 : 1;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
