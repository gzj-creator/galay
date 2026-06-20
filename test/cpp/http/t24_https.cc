/**
 * @file T24-SimpleHttpsTest.cc
 * @brief 简单的 HTTPS 测试 - 用于调试
 */

#include <galay/cpp/galay-http/client/http_client.h>
#include <galay/cpp/galay-http/builder/http_builder.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <iostream>
#include <atomic>

using namespace galay::http;
using namespace galay::kernel;

#ifdef GALAY_SSL_FEATURE_ENABLED

std::atomic<int> g_success{0};
std::atomic<int> g_fail{0};

Task<void> singleRequest(int id) {
    std::cout << "[Request " << id << "] Starting..." << std::endl;

    HttpsClient client(HttpsClientBuilder()
        .verifyPeer(false)
        .build());

    try {
        // 连接
        std::cout << "[Request " << id << "] Connecting..." << std::endl;
        auto connect_result = co_await client.connect("https://localhost:8443/");
        if (!connect_result) {
            std::cerr << "[Request " << id << "] Connect failed: " << connect_result.error().message() << std::endl;
            g_fail++;
            co_return;
        }
        std::cout << "[Request " << id << "] Connected" << std::endl;

        // SSL 握手
        std::cout << "[Request " << id << "] Handshaking..." << std::endl;
        auto handshake_result = co_await client.handshake();
        if (!handshake_result) {
            std::cerr << "[Request " << id << "] Handshake failed: " << handshake_result.error().message() << std::endl;
            g_fail++;
            co_await client.close();
            co_return;
        }
        std::cout << "[Request " << id << "] Handshake completed" << std::endl;

        auto session_result = client.getSession();
        if (!session_result) {
            co_await client.close();
            co_return;
        }
        auto& session = *session_result.value();
        // 发送请求
        std::cout << "[Request " << id << "] Sending request..." << std::endl;
        auto request = Http1_1RequestBuilder::get("/")
            .host("localhost")
            .connection("close")
            .buildMove();

        auto& writer = session.getWriter();
        while (true) {
            auto send_result = co_await writer.sendRequest(request);
            if (!send_result) {
                std::cerr << "[Request " << id << "] Send failed: " << send_result.error().message() << std::endl;
                g_fail++;
                co_await client.close();
                co_return;
            }
            if (send_result.value()) break;
        }
        std::cout << "[Request " << id << "] Request sent" << std::endl;

        // 接收响应
        std::cout << "[Request " << id << "] Receiving response..." << std::endl;
        HttpResponse response;
        auto& reader = session.getReader();
        while (true) {
            auto recv_result = co_await reader.getResponse(response);
            if (!recv_result) {
                std::cerr << "[Request " << id << "] Recv failed: " << recv_result.error().message() << std::endl;
                g_fail++;
                co_await client.close();
                co_return;
            }
            if (recv_result.value()) break;
        }

        std::cout << "[Request " << id << "] Response received: " << static_cast<int>(response.header().code()) << std::endl;

        // 验证响应
        if (static_cast<int>(response.header().code()) == 200) {
            g_success++;
            std::cout << "[Request " << id << "] SUCCESS" << std::endl;
        } else {
            g_fail++;
            std::cout << "[Request " << id << "] FAILED - wrong status code" << std::endl;
        }

        co_await client.close();

    } catch (const std::exception& e) {
        std::cerr << "[Request " << id << "] Exception: " << e.what() << std::endl;
        g_fail++;
    }

    co_return;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "简单 HTTPS 测试 (调试用)" << std::endl;
    std::cout << "==========================================" << std::endl;

    // 创建运行时
    Runtime rt = RuntimeBuilder().ioSchedulerCount(2).computeSchedulerCount(0).build();
    rt.start();

    // 发送 20 个顺序请求
    for (int i = 0; i < 20; i++) {
        auto* scheduler = rt.getNextIOScheduler();
        if (scheduler) {
            scheduleTask(scheduler, singleRequest(i));
        }
        // 等待每个请求完成
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 等待所有请求完成
    std::this_thread::sleep_for(std::chrono::seconds(5));

    rt.stop();

    std::cout << "\n==========================================" << std::endl;
    std::cout << "测试完成" << std::endl;
    std::cout << "成功: " << g_success << ", 失败: " << g_fail << std::endl;
    std::cout << "==========================================" << std::endl;

    return g_fail.load() == 0 && g_success.load() == 20 ? 0 : 1;
}

#else

int main() {
    std::cout << "SSL support is not enabled." << std::endl;
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON" << std::endl;
    return 0;
}

#endif
