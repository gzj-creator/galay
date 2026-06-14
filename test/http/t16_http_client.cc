/**
 * @file test_http_client_timeout.cc
 * @brief HttpClient 超时和断连测试
 * @details 测试 HttpClientAwaitable 的超时功能和连接断开处理
 */

#include "http/client/http_client.h"
#include "kernel/kernel/runtime.h"
#include <chrono>
#include <thread>

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;
using namespace std::chrono_literals;

// 测试服务器配置
constexpr const char* TEST_HOST = "127.0.0.1";
constexpr uint16_t TEST_PORT = 8080;

/**
 * @brief 测试：请求超时
 * @details 服务器延迟响应，客户端设置较短超时时间
 */
Task<void> testRequestTimeout(IOScheduler* scheduler)
{

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            co_return;
        }

        // 创建 HttpClient
        HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

        // 获取Session
        auto session_result = client.getSession();
        if (!session_result) {
            co_await client.close();
            co_return;
        }
        auto& session = *session_result.value();

        // 发送请求并设置 1 秒超时（假设服务器会延迟 5 秒响应）

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await session.get("/delay/5").timeout(1000ms);

            if (!result) {
                // 期望超时错误
                if (result.error().code() == kRequestTimeOut || result.error().code() == kRecvTimeOut) {
                } else {
                }
                break;
            } else if (result.value().has_value()) {
                break;
            }
            // std::nullopt，继续循环
        }

        // 关闭连接
        co_await client.close();

    } catch (const std::exception& e) {
    }
    co_return;
}

/**
 * @brief 测试：连接超时
 * @details 连接到不存在的服务器，测试连接超时
 */
Task<void> testConnectTimeout(IOScheduler* scheduler)
{

    try {
        // 尝试连接到不存在的服务器（使用不可路由的 IP）
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            co_return;
        }

        Host host(IPType::IPV4, "192.0.2.1", 9999);
        auto connectResult = co_await socket.connect(host).timeout(2000ms);

        if (!connectResult) {
            if (connectResult.error().code() == kTimeout) {
            } else {
            }
        } else {
        }

    } catch (const std::exception& e) {
    }

    co_return;
}

/**
 * @brief 测试：服务器主动断开连接
 * @details 服务器在发送部分数据后断开连接
 */
Task<void> testServerDisconnect(IOScheduler* scheduler)
{

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            co_return;
        }

        // 创建 HttpClient
        HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

        // 获取Session
        auto session_result = client.getSession();
        if (!session_result) {
            co_await client.close();
            co_return;
        }
        auto& session = *session_result.value();

        // 请求一个会导致服务器断开连接的端点

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await session.get("/disconnect");

            if (!result) {
                // 期望连接错误
                break;
            } else if (result.value().has_value()) {
                break;
            }
            // std::nullopt，继续循环
        }

        // 尝试关闭连接（可能已经断开）
        auto closeResult = co_await client.close();
        if (closeResult) {
        } else {
        }

    } catch (const std::exception& e) {
    }

    co_return;
}

/**
 * @brief 测试：接收超时
 * @details 服务器发送部分数据后停止，测试接收超时
 */
Task<void> testReceiveTimeout(IOScheduler* scheduler)
{

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            co_return;
        }

        // 创建 HttpClient
        HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

        // 请求一个会发送部分数据然后停止的端点
        auto session_result = client.getSession();
        if (!session_result) {
            co_await client.close();
            co_return;
        }
        auto& session = *session_result.value();

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await session.get("/partial").timeout(2000ms);

            if (!result) {
                if (result.error().code() == kRequestTimeOut || result.error().code() == kRecvTimeOut) {
                } else {
                }
                break;
            } else if (result.value().has_value()) {
                break;
            }
            // std::nullopt，继续循环
        }

        // 关闭连接
        co_await client.close();

    } catch (const std::exception& e) {
    }
    co_return;
}

/**
 * @brief 测试：多次超时重试
 * @details 测试超时后重新发起请求
 */
Task<void> testTimeoutRetry(IOScheduler* scheduler)
{

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            co_return;
        }

        // 创建 HttpClient
        HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

        auto session_result = client.getSession();
        if (!session_result) {
            co_await client.close();
            co_return;
        }
        auto& session = *session_result.value();

        // 第一次请求：超时
        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result1 = co_await session.get("/delay/5").timeout(1000ms);

            if (!result1) {
                if (result1.error().code() == kRequestTimeOut || result1.error().code() == kRecvTimeOut) {
                } else {
                }
                break;
            } else if (result1.value().has_value()) {
                break;
            }
        }
        // 第二次请求：正常完成
        loop_count = 0;
        while (true) {
            loop_count++;
            auto result2 = co_await session.get("/api/data").timeout(5000ms);

            if (!result2) {
                break;
            } else if (result2.value().has_value()) {
                auto& response = result2.value().value();
                break;
            }
        }

        // 关闭连接
        co_await client.close();

    } catch (const std::exception& e) {
    }
    co_return;
}

/**
 * @brief 测试：正常请求（无超时）
 * @details 验证超时功能不影响正常请求
 */
Task<void> testNormalRequestWithTimeout(IOScheduler* scheduler)
{

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            co_return;
        }

        // 创建 HttpClient
        HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

        // 发送正常请求，设置足够长的超时
        auto session_result = client.getSession();
        if (!session_result) {
            co_await client.close();
            co_return;
        }
        auto& session = *session_result.value();
        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await session.get("/api/data").timeout(5000ms);

            if (!result) {
                break;
            } else if (result.value().has_value()) {
                auto& response = result.value().value();
                break;
            }
            // std::nullopt，继续循环
        }

        // 关闭连接
        co_await client.close();

    } catch (const std::exception& e) {
    }
    co_return;
}

/**
 * @brief 主函数
 */
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
        scheduleTask(scheduler, testNormalRequestWithTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, testRequestTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, testConnectTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, testServerDisconnect(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, testReceiveTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, testTimeoutRetry(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(5));

        runtime.stop();


    } catch (const std::exception& e) {
        return 1;
    }

    return 0;
}
