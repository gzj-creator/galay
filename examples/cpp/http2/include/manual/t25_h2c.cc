/**
 * @file T25-H2cClient.cc
 * @brief H2c 客户端测试程序
 * @details 用于测试 H2c 客户端功能
 *
 * 使用方法:
 *   ./test/T25-H2cClient <host> <port> [requests]
 *   默认: localhost 9080 10
 */

#include <galay/cpp/galay-http2/client/h2c_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <iostream>
#include <atomic>
#include <cstdlib>

using namespace galay::http2;
using namespace galay::kernel;

std::atomic<int> success_count{0};
std::atomic<int> fail_count{0};
std::atomic<bool> g_done{false};

/**
 * @brief 客户端测试协程
 */
Task<void> test_client(const std::string& host, uint16_t port, int num_requests) {
    H2cClient client(H2cClientBuilder().build());

    std::cout << "Connecting to " << host << ":" << port << "..." << std::endl;
    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << std::endl;
        fail_count++;
        g_done = true;
        co_return;
    }
    std::cout << "Connected!" << std::endl;

    // Upgrade to HTTP/2
    std::cout << "Upgrading to HTTP/2..." << std::endl;
    auto upgrade_result = co_await client.upgrade("/");
    if (!upgrade_result) {
        std::cerr << "Upgrade failed: " << upgrade_result.error().toString() << std::endl;
        fail_count++;
        g_done = true;
        co_return;
    }
    std::cout << "Upgrade successful!" << std::endl;

    // Send multiple GET requests
    std::cout << "Sending " << num_requests << " requests..." << std::endl;
    for (int i = 0; i < num_requests; i++) {
        std::cout << "Starting request " << (i + 1) << "..." << std::endl;

        auto stream = client.get("/");
        if (!stream) {
            fail_count++;
            continue;
        }
        bool finished = false;
        while (!finished) {
            auto frame_result = co_await stream->getFrame();
            if (!frame_result || !frame_result.value()) {
                fail_count++;
                break;
            }
            auto frame = std::move(frame_result.value());
            if ((frame->isHeaders() || frame->isData()) && frame->isEndStream()) {
                finished = true;
            }
        }
        if (finished) {
            std::cout << "Request " << (i + 1) << " completed with frame stream." << std::endl;
            success_count++;
        }

        std::cout << "Request " << (i + 1) << " finished, continuing to next..." << std::endl;
    }

    // Close connection
    co_await client.shutdown();

    g_done = true;
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 9080;
    int num_requests = 10;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    if (argc > 3) num_requests = std::atoi(argv[3]);

    std::cout << "========================================\n";
    std::cout << "H2c Client Test\n";
    std::cout << "========================================\n";
    std::cout << "Target: " << host << ":" << port << "\n";
    std::cout << "Requests: " << num_requests << "\n";
    std::cout << "========================================\n\n";

    try {
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        runtime.start();

        auto join = runtime.spawn(test_client(host, port, num_requests));
        if (!join) {
            std::cerr << "Failed to spawn client coroutine: " << join.error().message() << "\n";
            runtime.stop();
            return 1;
        }

        // Wait for coroutine completion (max 30 seconds)
        for (int i = 0; i < 300; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (g_done.load()) {
                break;
            }
        }

        if (g_done.load()) {
            auto join_result = join->join();
            if (!join_result) {
                std::cerr << "Client coroutine join failed: "
                          << join_result.error().message() << "\n";
                runtime.stop();
                return 1;
            }
        }

        runtime.stop();

        std::cout << "\n========================================\n";
        std::cout << "Test Results:\n";
        std::cout << "========================================\n";
        std::cout << "Success: " << success_count << "\n";
        std::cout << "Failed: " << fail_count << "\n";
        std::cout << "Total: " << (success_count + fail_count) << "/" << num_requests << "\n";
        std::cout << "========================================\n";

        if (!g_done.load()) {
            std::cerr << "Client coroutine timeout.\n";
            return 1;
        }
        return (fail_count > 0) ? 1 : 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
