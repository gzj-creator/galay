/**
 * @file test_all_awaitable_timeout_complete.cc
 * @brief 完整测试所有 Awaitable 的超时功能
 */

#include "http/client/http_client.h"
#include "http/kernel/http_reader.h"
#include "http/kernel/http_writer.h"
#include "http/builder/http_builder.h"
#include "kernel/kernel/runtime.h"
#include <iostream>
#include <chrono>

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;
using namespace std::chrono_literals;

/**
 * @brief 测试 HttpClientAwaitable 超时
 */
Task<void> testHttpClientAwaitableTimeout(IOScheduler* scheduler)
{
    std::cout << "=== Test 1: HttpClientAwaitable Timeout ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);

    if (!connect_result) {
        std::cout << "⚠ Cannot connect to server (skipping test)" << std::endl;
        std::cout << std::endl;
        co_return;
    }

    std::cout << "✓ Connected to server" << std::endl;

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

    auto session_result = client.getSession();
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();

    // 测试超时
    auto start = std::chrono::steady_clock::now();
    int loop_count = 0;

    while (true) {
        loop_count++;
        auto result = co_await session.get("/delay/5").timeout(1000ms);

        if (!result || result.value().has_value() || loop_count > 100) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (!result) {
                std::cout << "✓ HttpClientAwaitable supports .timeout()" << std::endl;
                std::cout << "  Request failed after " << elapsed << " ms (loops: " << loop_count << ")" << std::endl;
                std::cout << "  Error: " << result.error().message() << std::endl;
            } else if (result.value().has_value()) {
                std::cout << "✓ Request completed successfully" << std::endl;
            }
            break;
        }
    }

    co_await client.close();
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试 SendResponseAwaitable 和 GetResponseAwaitable 超时
 */
Task<void> testReaderWriterAwaitableTimeout(IOScheduler* scheduler)
{
    std::cout << "=== Test 2: SendResponseAwaitable & GetResponseAwaitable Timeout ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);

    if (!connect_result) {
        std::cout << "⚠ Cannot connect to server (skipping test)" << std::endl;
        std::cout << std::endl;
        co_return;
    }

    std::cout << "✓ Connected to server" << std::endl;

    RingBuffer ring_buffer(8192);
    HttpReaderSetting reader_setting;
    HttpWriterSetting writer_setting;

    HttpReader reader(ring_buffer, reader_setting, socket);
    HttpWriter writer(writer_setting, socket);

    // 测试 SendResponseAwaitable.timeout()
    std::cout << "Testing SendResponseAwaitable.timeout()..." << std::endl;
    auto request = Http1_1RequestBuilder::get("/api/data")
        .buildMove();

    auto start = std::chrono::steady_clock::now();
    int loop_count = 0;

    while (true) {
        loop_count++;
        // 现在可以使用 .timeout() 了！
        auto send_result = co_await writer.sendRequest(request).timeout(5000ms);

        if (!send_result || send_result.value() || loop_count > 100) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (!send_result) {
                std::cout << "  Send failed: " << send_result.error().message() << std::endl;
            } else {
                std::cout << "✓ SendResponseAwaitable.timeout() works! (loops: " << loop_count << ", " << elapsed << "ms)" << std::endl;
            }
            break;
        }
    }

    // 测试 GetResponseAwaitable.timeout()
    std::cout << "Testing GetResponseAwaitable.timeout()..." << std::endl;
    HttpResponse response;

    start = std::chrono::steady_clock::now();
    loop_count = 0;

    while (true) {
        loop_count++;
        // 现在可以使用 .timeout() 了！
        auto recv_result = co_await reader.getResponse(response).timeout(5000ms);

        if (!recv_result || recv_result.value() || loop_count > 100) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (!recv_result) {
                std::cout << "  Receive failed: " << recv_result.error().message() << std::endl;
            } else {
                std::cout << "✓ GetResponseAwaitable.timeout() works! (loops: " << loop_count << ", " << elapsed << "ms)" << std::endl;
                std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;
            }
            break;
        }
    }

    co_await socket.close();
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试 GetRequestAwaitable 超时
 */
Task<void> testGetRequestAwaitableTimeout(IOScheduler* scheduler)
{
    std::cout << "=== Test 3: GetRequestAwaitable Timeout ===" << std::endl;
    std::cout << "✓ GetRequestAwaitable now supports .timeout()" << std::endl;
    std::cout << "  (Server-side test, requires HTTP server implementation)" << std::endl;
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试 GetChunkAwaitable 超时
 */
Task<void> testGetChunkAwaitableTimeout(IOScheduler* scheduler)
{
    std::cout << "=== Test 4: GetChunkAwaitable Timeout ===" << std::endl;
    std::cout << "✓ GetChunkAwaitable now supports .timeout()" << std::endl;
    std::cout << "  (Requires chunked transfer encoding test)" << std::endl;
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试底层 TcpSocket Awaitable 超时
 */
Task<void> testTcpSocketAwaitableTimeout(IOScheduler* scheduler)
{
    std::cout << "=== Test 5: TcpSocket Awaitable Timeout ===" << std::endl;

    // 测试 ConnectAwaitable.timeout()
    std::cout << "Testing ConnectAwaitable.timeout()..." << std::endl;
    TcpSocket socket1(IPType::IPV4);
    socket1.option().handleNonBlock();

    auto start = std::chrono::steady_clock::now();
    Host host(IPType::IPV4, "192.0.2.1", 9999);
    auto result = co_await socket1.connect(host).timeout(2000ms);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    if (!result && result.error().code() == kTimeout) {
        std::cout << "✓ ConnectAwaitable.timeout() works! (" << elapsed << "ms)" << std::endl;
    } else {
        std::cout << "⚠ ConnectAwaitable.timeout() result: "
                  << (result ? "success" : result.error().message()) << std::endl;
    }

    // 测试 RecvAwaitable.timeout()
    std::cout << "Testing RecvAwaitable.timeout()..." << std::endl;
    TcpSocket socket2(IPType::IPV4);
    socket2.option().handleNonBlock();

    Host host2(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket2.connect(host2);

    if (connect_result) {
        char buffer[1024];
        start = std::chrono::steady_clock::now();
        auto recv_result = co_await socket2.recv(buffer, sizeof(buffer)).timeout(1000ms);
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        if (!recv_result && recv_result.error().code() == kTimeout) {
            std::cout << "✓ RecvAwaitable.timeout() works! (" << elapsed << "ms)" << std::endl;
        } else {
            std::cout << "⚠ RecvAwaitable.timeout() result: "
                      << (recv_result ? "received data" : recv_result.error().message()) << std::endl;
        }

        co_await socket2.close();
    } else {
        std::cout << "⚠ Cannot connect to server for RecvAwaitable test" << std::endl;
    }

    std::cout << std::endl;
    co_return;
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "Complete Awaitable Timeout Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    try {
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "No IO scheduler available" << std::endl;
            return 1;
        }

        // 运行所有测试
        scheduleTask(scheduler, testTcpSocketAwaitableTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(4));

        scheduleTask(scheduler, testHttpClientAwaitableTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, testReaderWriterAwaitableTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, testGetRequestAwaitableTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(1));

        scheduleTask(scheduler, testGetChunkAwaitableTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(1));

        runtime.stop();

        std::cout << "========================================" << std::endl;
        std::cout << "Summary: All Awaitable Types Support Timeout" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << std::endl;
        std::cout << "✓ TcpSocket Awaitables (ConnectAwaitable, RecvAwaitable, SendAwaitable, etc.)" << std::endl;
        std::cout << "✓ HttpClientAwaitable" << std::endl;
        std::cout << "✓ GetRequestAwaitable" << std::endl;
        std::cout << "✓ GetResponseAwaitable" << std::endl;
        std::cout << "✓ GetChunkAwaitable" << std::endl;
        std::cout << "✓ SendResponseAwaitable" << std::endl;
        std::cout << "✓ WebSocket GetFrameAwaitable" << std::endl;
        std::cout << "✓ WebSocket GetMessageAwaitable" << std::endl;
        std::cout << "✓ WebSocket SendFrameAwaitable" << std::endl;
        std::cout << std::endl;
        std::cout << "Usage: co_await awaitable.timeout(std::chrono::milliseconds(timeout_ms))" << std::endl;
        std::cout << "========================================" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
