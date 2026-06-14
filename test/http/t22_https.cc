/**
 * @file T22-HttpsClient.cc
 * @brief HTTPS 客户端测试
 */

#include "http/client/http_client.h"
#include "http/builder/http_builder.h"
#include "kernel/kernel/runtime.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace galay::http;
using namespace galay::kernel;

#ifdef GALAY_SSL_FEATURE_ENABLED

static std::atomic<bool> g_done{false};
static std::atomic<bool> g_success{false};
static std::atomic<bool> g_failed{false};

Task<void> testHttpsClient() {
    std::cout << "=== HTTPS Client Test ===" << std::endl;

    HttpsClient client(HttpsClientBuilder()
        .verifyPeer(false)  // 测试时不验证证书
        .build());

    try {
        // 连接到本地 HTTPS 服务器
        std::cout << "Connecting to https://localhost:8443/..." << std::endl;
        auto connect_result = co_await client.connect("https://localhost:8443/");
        if (!connect_result) {
            std::cerr << "Connect failed: " << connect_result.error().message() << std::endl;
            g_failed = true;
            g_done = true;
            co_return;
        }
        std::cout << "TCP connection established" << std::endl;

        // SSL 握手
        std::cout << "Performing SSL handshake..." << std::endl;
        auto handshake_result = co_await client.handshake();
        if (!handshake_result) {
            std::cerr << "SSL handshake failed: " << handshake_result.error().message() << std::endl;
            g_failed = true;
            co_await client.close();
            g_done = true;
            co_return;
        }
        std::cout << "SSL handshake completed" << std::endl;

        // 发送 GET 请求
        std::cout << "Sending GET request..." << std::endl;
        auto request = Http1_1RequestBuilder::get("/")
            .host("localhost")
            .connection("close")
            .buildMove();

        auto session_result = client.getSession();
        if (!session_result) {
            co_await client.close();
            co_return;
        }
        auto& session = *session_result.value();

        auto& writer = session.getWriter();
        while (true) {
            auto send_result = co_await writer.sendRequest(request);
            if (!send_result) {
                std::cerr << "Send failed: " << send_result.error().message() << std::endl;
                g_failed = true;
                co_await client.close();
                g_done = true;
                co_return;
            }
            if (send_result.value()) break;
        }
        std::cout << "Request sent" << std::endl;

        // 接收响应
        std::cout << "Receiving response..." << std::endl;
        HttpResponse response;
        auto& reader = session.getReader();
        int recv_attempts = 0;
        while (true) {
            recv_attempts++;
            std::cout << "  Recv attempt " << recv_attempts << "..." << std::endl;
            auto recv_result = co_await reader.getResponse(response);
            if (!recv_result) {
                auto& err = recv_result.error();
                std::cerr << "Recv failed (attempt " << recv_attempts << "): "
                         << recv_result.error().message()
                         << " (code: " << static_cast<int>(err.code()) << ")" << std::endl;

                // 如果是连接关闭，检查是否已经收到部分响应
                if (static_cast<int>(response.header().code()) != 0) {
                    std::cout << "Partial response received before connection closed" << std::endl;
                }
                g_failed = true;
                break;
            }
            if (recv_result.value()) {
                std::cout << "  Response complete!" << std::endl;
                break;
            }
            std::cout << "  Need more data..." << std::endl;
        }

        std::cout << "Response received:" << std::endl;
        std::cout << "  Complete: " << (response.isComplete() ? "yes" : "no") << std::endl;
        std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;
        std::cout << "  Body length: " << response.getBodyStr().size() << std::endl;
        std::cout << "  Body: " << response.getBodyStr() << std::endl;

        if (response.isComplete() && static_cast<int>(response.header().code()) == 200) {
            g_success = true;
        } else {
            g_failed = true;
        }

        co_await client.close();
        std::cout << "=== HTTPS Client Test Completed ===" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        g_failed = true;
    }

    g_done = true;
    co_return;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "HTTPS Client Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Note: Start T21-HttpsServer first!" << std::endl;
    std::cout << std::endl;

    // 创建运行时
    Runtime rt = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    rt.start();

    auto* scheduler = rt.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "No IO scheduler available" << std::endl;
        return 1;
    }

    scheduleTask(scheduler, testHttpsClient());

    // 等待测试完成
    while (!g_done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    rt.stop();

    return g_success.load() && !g_failed.load() ? 0 : 1;
}

#else

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "HTTPS Client Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "SSL support is not enabled." << std::endl;
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON" << std::endl;
    return 0;
}

#endif
