/**
 * @file test_http_client_awaitable_edge_cases.cc
 * @brief HttpClientAwaitable 边界测试
 */

#include <iostream>
#include "galay-http/client/http_client.h"
#include "galay-kernel/core/runtime.h"

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief 测试1: 连接失败
 */
Task<void> test_connection_failure(IOScheduler* scheduler)
{

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    // 连接到不存在的服务器
    Host host(IPType::IPV4, "127.0.0.1", 9999);
    auto connect_result = co_await socket.connect(host);

    if (!connect_result) {
    } else {
    }

    co_return;
}

/**
 * @brief 测试2: 服务器关闭连接
 */
Task<void> test_server_close_connection(IOScheduler* scheduler)
{

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        co_return;
    }

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());
    auto session_result = client.getSession();
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();
    // 发送请求后立即关闭连接
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.get("/");

        if (!result) {
            break;
        }

        if (result.value().has_value()) {
            break;
        }

        if (loop_count > 100) {
            break;
        }
    }

    co_await client.close();
    co_return;
}

/**
 * @brief 测试3: 多个连续请求
 */
Task<void> test_multiple_requests(IOScheduler* scheduler)
{

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        co_return;
    }

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());
    auto session_result = client.getSession();
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();
    // 发送3个连续请求
    for (int i = 0; i < 3; i++) {

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await session.get("/api/info");

            if (!result) {
                co_await client.close();
                co_return;
            }

            if (result.value().has_value()) {
                HttpResponse response = std::move(result.value().value());
                break;
            }

            if (loop_count > 100) {
                co_await client.close();
                co_return;
            }
        }
    }

    co_await client.close();
    co_return;
}

/**
 * @brief 测试4: 大请求体
 */
Task<void> test_large_request_body(IOScheduler* scheduler)
{

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        co_return;
    }

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());
    auto session_result = client.getSession();
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();

    // 创建一个大的请求体 (10KB)
    std::string large_body(10240, 'A');

    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.post("/api/data", large_body, "text/plain");

        if (!result) {
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            break;
        }

        if (loop_count > 100) {
            break;
        }
    }

    co_await client.close();
    co_return;
}

/**
 * @brief 测试5: 404 错误
 */
Task<void> test404NotFound(IOScheduler* scheduler)
{

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        co_return;
    }

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
        auto result = co_await session.get("/nonexistent");

        if (!result) {
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            auto status_code = static_cast<int>(response.header().code());
            if (status_code == 404) {
            } else {
            }
            break;
        }

        if (loop_count > 100) {
            break;
        }
    }

    co_await client.close();
    co_return;
}

/**
 * @brief 测试6: 空响应体
 */
Task<void> test_empty_response(IOScheduler* scheduler)
{

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        co_return;
    }

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
        auto result = co_await session.del("/api/resource");

        if (!result) {
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            break;
        }

        if (loop_count > 100) {
            break;
        }
    }

    co_await client.close();
    co_return;
}

int main()
{

    try {
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            return 1;
        }

        // 运行边界测试
        scheduleTask(scheduler, test_connection_failure(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, test_server_close_connection(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, test_multiple_requests(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, test_large_request_body(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, test404NotFound(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, test_empty_response(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        runtime.stop();


    } catch (const std::exception& e) {
        return 1;
    }

    return 0;
}
