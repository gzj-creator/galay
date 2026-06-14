/**
 * @file E6-HttpsClient.cc
 * @brief HTTPS 客户端示例
 * @details 演示如何使用 HttpsClient 发送 HTTPS 请求，支持 Keep-Alive 连接复用
 */

#include "http/client/http_client.h"
#include "http/builder/http_builder.h"
#include "kernel/kernel/runtime.h"
#include <iostream>

using namespace galay::http;
using namespace galay::kernel;

#ifdef GALAY_SSL_FEATURE_ENABLED

Task<void> httpsClientExample(const std::string& url) {
    std::cout << "Connecting to " << url << "...\n";

    HttpsClient client(HttpsClientBuilder()
        .verifyPeer(false)  // 测试时不验证证书
        .build());

    try {
        // 连接
        auto connect_result = co_await client.connect(url);
        if (!connect_result) {
            std::cerr << "Connect failed: " << connect_result.error().message() << "\n";
            co_return;
        }
        std::cout << "TCP connection established\n";

        // SSL 握手
        auto handshake_result = co_await client.handshake();
        if (!handshake_result) {
            std::cerr << "Handshake failed: " << handshake_result.error().message() << "\n";
            co_await client.close();
            co_return;
        }
        std::cout << "SSL handshake completed\n";
        auto session_result = client.getSession();
        if (!session_result) {
            (void)co_await client.close();
            co_return;
        }
        auto& session = *session_result.value();
        auto& writer = session.getWriter();
        auto& reader = session.getReader();

        // 发送多个请求 (Keep-Alive)
        for (int i = 1; i <= 3; i++) {
            std::cout << "\n--- Request " << i << " ---\n";

            // 构建请求
            auto request = Http1_1RequestBuilder::get("/")
                .host("localhost")
                .connection("keep-alive")
                .buildMove();

            // 发送请求
            while (true) {
                auto send_result = co_await writer.sendRequest(request);
                if (!send_result) {
                    std::cerr << "Send failed: " << send_result.error().message() << "\n";
                    co_await client.close();
                    co_return;
                }
                if (send_result.value()) break;
            }
            std::cout << "Request sent\n";

            // 接收响应
            HttpResponse response;
            while (true) {
                auto recv_result = co_await reader.getResponse(response);
                if (!recv_result) {
                    std::cerr << "Recv failed: " << recv_result.error().message() << "\n";
                    co_await client.close();
                    co_return;
                }
                if (recv_result.value()) break;
            }

            std::cout << "Response: " << static_cast<int>(response.header().code()) << "\n";
            std::cout << "Body length: " << response.getBodyStr().size() << " bytes\n";

            // 重置 response
            response = HttpResponse();
        }

        co_await client.close();
        std::cout << "\nConnection closed\n";

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    co_return;
}

int main(int argc, char* argv[]) {
    std::string url = "https://localhost:8443/";
    if (argc > 1) url = argv[1];

    std::cout << "========================================\n";
    std::cout << "HTTPS Client Example\n";
    std::cout << "========================================\n";

    Runtime rt = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    rt.start();

    auto join = rt.spawn(httpsClientExample(url));
    if (!join) {
        rt.stop();
        return 1;
    }
    auto join_result = join->join();
    if (!join_result) {
        rt.stop();
        return 1;
    }

    rt.stop();

    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
