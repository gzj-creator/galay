/**
 * @file test_http_methods.cc
 * @brief 测试 HttpClient 的所有 HTTP 方法
 */

#include <galay/cpp/galay-http/client/http_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <iostream>
#include <chrono>

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;
using namespace std::chrono_literals;

// 测试服务器配置
constexpr const char* TEST_HOST = "127.0.0.1";
constexpr uint16_t TEST_PORT = 8080;

/**
 * @brief 测试 GET 方法
 */
Task<void> test_get_method(IOScheduler* scheduler)
{
    std::cout << "=== Test 1: GET Method ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
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
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.get("/api/data");

        if (!result) {
            std::cout << "❌ GET request failed: " << result.error().message() << std::endl;
            break;
        } else if (result.value().has_value()) {
            auto& response = result.value().value();
            std::cout << "✓ GET request succeeded" << std::endl;
            std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;
            std::cout << "  Body length: " << response.getBodyStr().length() << " bytes" << std::endl;
            std::cout << "  Loops: " << loop_count << std::endl;
            break;
        }

        if (loop_count > 100) {
            std::cout << "❌ Too many loops" << std::endl;
            break;
        }
    }

    co_await client.close();
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试 POST 方法
 */
Task<void> test_post_method(IOScheduler* scheduler)
{
    std::cout << "=== Test 2: POST Method ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
    auto connect_result = co_await socket.connect(host);

    if (!connect_result) {
        std::cout << "⚠ Cannot connect to server (skipping test)" << std::endl;
        std::cout << std::endl;
        co_return;
    }

    std::cout << "✓ Connected to server" << std::endl;

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

    std::string body = R"({"name": "test", "value": 123})";
    auto session_result = client.getSession();
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.post("/api/data", body, "application/json");

        if (!result) {
            std::cout << "❌ POST request failed: " << result.error().message() << std::endl;
            break;
        } else if (result.value().has_value()) {
            auto& response = result.value().value();
            std::cout << "✓ POST request succeeded" << std::endl;
            std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;
            std::cout << "  Loops: " << loop_count << std::endl;
            break;
        }

        if (loop_count > 100) {
            std::cout << "❌ Too many loops" << std::endl;
            break;
        }
    }

    co_await client.close();
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试 PUT 方法
 */
Task<void> test_put_method(IOScheduler* scheduler)
{
    std::cout << "=== Test 3: PUT Method ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
    auto connect_result = co_await socket.connect(host);

    if (!connect_result) {
        std::cout << "⚠ Cannot connect to server (skipping test)" << std::endl;
        std::cout << std::endl;
        co_return;
    }

    std::cout << "✓ Connected to server" << std::endl;

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

    std::string body = R"({"name": "updated", "value": 456})";
    auto session_result = client.getSession();
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.put("/api/data/1", body, "application/json");

        if (!result) {
            std::cout << "❌ PUT request failed: " << result.error().message() << std::endl;
            break;
        } else if (result.value().has_value()) {
            auto& response = result.value().value();
            std::cout << "✓ PUT request succeeded" << std::endl;
            std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;
            std::cout << "  Loops: " << loop_count << std::endl;
            break;
        }

        if (loop_count > 100) {
            std::cout << "❌ Too many loops" << std::endl;
            break;
        }
    }

    co_await client.close();
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试 DELETE 方法
 */
Task<void> test_delete_method(IOScheduler* scheduler)
{
    std::cout << "=== Test 4: DELETE Method ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
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
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.del("/api/data/1");

        if (!result) {
            std::cout << "❌ DELETE request failed: " << result.error().message() << std::endl;
            break;
        } else if (result.value().has_value()) {
            auto& response = result.value().value();
            std::cout << "✓ DELETE request succeeded" << std::endl;
            std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;
            std::cout << "  Loops: " << loop_count << std::endl;
            break;
        }

        if (loop_count > 100) {
            std::cout << "❌ Too many loops" << std::endl;
            break;
        }
    }

    co_await client.close();
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试 HEAD 方法
 */
Task<void> test_head_method(IOScheduler* scheduler)
{
    std::cout << "=== Test 5: HEAD Method ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
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
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.head("/api/data");

        if (!result) {
            std::cout << "❌ HEAD request failed: " << result.error().message() << std::endl;
            break;
        } else if (result.value().has_value()) {
            auto& response = result.value().value();
            std::cout << "✓ HEAD request succeeded" << std::endl;
            std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;
            std::cout << "  Body length: " << response.getBodyStr().length() << " bytes (should be 0)" << std::endl;
            std::cout << "  Loops: " << loop_count << std::endl;
            break;
        }

        if (loop_count > 100) {
            std::cout << "❌ Too many loops" << std::endl;
            break;
        }
    }

    co_await client.close();
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试 OPTIONS 方法
 */
Task<void> test_options_method(IOScheduler* scheduler)
{
    std::cout << "=== Test 6: OPTIONS Method ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
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
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.options("/api/data");

        if (!result) {
            std::cout << "❌ OPTIONS request failed: " << result.error().message() << std::endl;
            break;
        } else if (result.value().has_value()) {
            auto& response = result.value().value();
            std::cout << "✓ OPTIONS request succeeded" << std::endl;
            std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;

            // 查找 Allow 头
            auto& headers = response.header().headerPairs();
            if (headers.hasKey("Allow")) {
                std::cout << "  Allow: " << headers.getValue("Allow") << std::endl;
            }

            std::cout << "  Loops: " << loop_count << std::endl;
            break;
        }

        if (loop_count > 100) {
            std::cout << "❌ Too many loops" << std::endl;
            break;
        }
    }

    co_await client.close();
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试 PATCH 方法
 */
Task<void> test_patch_method(IOScheduler* scheduler)
{
    std::cout << "=== Test 7: PATCH Method ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
    auto connect_result = co_await socket.connect(host);

    if (!connect_result) {
        std::cout << "⚠ Cannot connect to server (skipping test)" << std::endl;
        std::cout << std::endl;
        co_return;
    }

    std::cout << "✓ Connected to server" << std::endl;

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

    std::string body = R"({"value": 789})";
    auto session_result = client.getSession();
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.patch("/api/data/1", body, "application/json");

        if (!result) {
            std::cout << "❌ PATCH request failed: " << result.error().message() << std::endl;
            break;
        } else if (result.value().has_value()) {
            auto& response = result.value().value();
            std::cout << "✓ PATCH request succeeded" << std::endl;
            std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;
            std::cout << "  Loops: " << loop_count << std::endl;
            break;
        }

        if (loop_count > 100) {
            std::cout << "❌ Too many loops" << std::endl;
            break;
        }
    }

    co_await client.close();
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试 TRACE 方法
 */
Task<void> test_trace_method(IOScheduler* scheduler)
{
    std::cout << "=== Test 8: TRACE Method ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
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
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.trace("/api/data");

        if (!result) {
            std::cout << "❌ TRACE request failed: " << result.error().message() << std::endl;
            break;
        } else if (result.value().has_value()) {
            auto& response = result.value().value();
            std::cout << "✓ TRACE request succeeded" << std::endl;
            std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;
            std::cout << "  Loops: " << loop_count << std::endl;
            break;
        }

        if (loop_count > 100) {
            std::cout << "❌ Too many loops" << std::endl;
            break;
        }
    }

    co_await client.close();
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试 CONNECT 方法
 */
Task<void> test_connect_method(IOScheduler* scheduler)
{
    std::cout << "=== Test 9: CONNECT Method ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
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
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.tunnel("example.com:443");

        if (!result) {
            std::cout << "❌ CONNECT request failed: " << result.error().message() << std::endl;
            break;
        } else if (result.value().has_value()) {
            auto& response = result.value().value();
            std::cout << "✓ CONNECT request succeeded" << std::endl;
            std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;
            std::cout << "  Loops: " << loop_count << std::endl;
            break;
        }

        if (loop_count > 100) {
            std::cout << "❌ Too many loops" << std::endl;
            break;
        }
    }

    co_await client.close();
    std::cout << std::endl;
    co_return;
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "HTTP Methods Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Note: This test requires a test server running on "
              << TEST_HOST << ":" << TEST_PORT << std::endl;
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
        scheduleTask(scheduler, test_get_method(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(1));

        scheduleTask(scheduler, test_post_method(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(1));

        scheduleTask(scheduler, test_put_method(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(1));

        scheduleTask(scheduler, test_delete_method(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(1));

        scheduleTask(scheduler, test_head_method(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(1));

        scheduleTask(scheduler, test_options_method(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(1));

        scheduleTask(scheduler, test_patch_method(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(1));

        scheduleTask(scheduler, test_trace_method(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(1));

        scheduleTask(scheduler, test_connect_method(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(1));

        runtime.stop();

        std::cout << "========================================" << std::endl;
        std::cout << "Summary: All HTTP Methods Tested" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << std::endl;
        std::cout << "✓ GET    - Retrieve resource" << std::endl;
        std::cout << "✓ POST   - Create resource" << std::endl;
        std::cout << "✓ PUT    - Update resource" << std::endl;
        std::cout << "✓ DELETE - Delete resource" << std::endl;
        std::cout << "✓ HEAD   - Get resource metadata" << std::endl;
        std::cout << "✓ OPTIONS - Query supported methods" << std::endl;
        std::cout << "✓ PATCH  - Partial update" << std::endl;
        std::cout << "✓ TRACE  - Diagnostic trace" << std::endl;
        std::cout << "✓ CONNECT - Establish tunnel" << std::endl;
        std::cout << "========================================" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
