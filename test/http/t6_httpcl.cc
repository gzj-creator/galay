/**
 * @file test_http_client_awaitable.cc
 * @brief HttpClientAwaitable 功能测试
 */

#include "galay-http/client/http_client.h"
#include "galay-kernel/core/runtime.h"

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief 测试 GET 请求
 */
Task<void> test_get(IOScheduler* scheduler)
{

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        co_return;
    }


    // 创建HttpClient
    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());
    auto session_result = client.getSession();
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();
    // 使用 HttpClientAwaitable API
    // 现在可以在循环中调用 client.get()，每次都会创建新的 awaitable
    int loop_count = 0;
    while (true) {
        loop_count++;

        auto result = co_await session.get("/api/info");

        if (!result) {
            // 错误处理
            break;
        }

        if (result.value().has_value()) {
            // 完成，获取响应
            HttpResponse response = std::move(result.value().value());
            break;
        }

        // std::nullopt，继续循环
    }

    co_await client.close();
    co_return;
}

/**
 * @brief 测试 POST 请求
 */
Task<void> test_post(IOScheduler* scheduler)
{

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        co_return;
    }


    // 创建HttpClient
    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

    // 使用 HttpClientAwaitable API 发送 POST 请求
    std::string body = R"({"name":"test","value":123})";
    int loop_count = 0;
    auto session_result = client.getSession();
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();
    while (true) {
        loop_count++;

        auto result = co_await session.post("/api/data", body, "application/json");

        if (!result) {
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            break;
        }

    }

    co_await client.close();
    co_return;
}

/**
 * @brief 测试多个连续请求
 */
Task<void> test_multiple_requests(IOScheduler* scheduler)
{

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        co_return;
    }


    // 创建HttpClient
    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

    // 发送多个请求
    std::vector<std::string> uris = {"/", "/hello", "/test"};
    auto session_result = client.getSession();
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();
    bool session_alive = true;
    for (const auto& uri : uris) {
        if (!session_alive) {
            break;
        }

        while (true) {
            auto result = co_await session.get(uri);

            if (!result) {
                session_alive = false;
                break;
            }

            if (result.value().has_value()) {
                HttpResponse response = std::move(result.value().value());
                break;
            }
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

        // 运行测试
        scheduleTask(scheduler, test_get(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, test_post(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, test_multiple_requests(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        runtime.stop();


    } catch (const std::exception& e) {
        return 1;
    }

    return 0;
}
