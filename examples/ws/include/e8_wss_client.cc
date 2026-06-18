/**
 * @file E8-WssClient.cc
 * @brief WSS (WebSocket Secure) 客户端示例
 * @details 演示如何使用 WssClient 连接到 WSS 服务器
 */

#include <iostream>
#include <chrono>
#include "galay-ws/client/ws_client.h"
#include "galay-ws/protoc/ws_frame.h"
#include "galay-kernel/core/runtime.h"

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

/**
 * @brief WSS 客户端协程
 */
Task<bool> wssClientTask(const std::string& url, int message_count) {
    try {
        constexpr auto kOpTimeout = std::chrono::milliseconds(3000);

        // 1. 创建 WssClient
        WssClient client(WssClientBuilder()
            .verifyPeer(false)  // 跳过证书验证（用于自签名证书）
            .build());

        // 2. TCP 连接
        auto connect_result = co_await client.connect(url);
        if (!connect_result) {
            co_return false;
        }

        // 3. SSL 握手
        auto handshake_result = co_await client.handshake();
        if (!handshake_result) {
            co_await client.close();
            co_return false;
        }

        // 4. 获取 Session 并升级 WebSocket
        auto session_result = client.getSession(WsWriterSetting::byClient());
        if (!session_result) {
            co_await client.close();
            co_return false;
        }
        auto& session = *session_result.value();
        auto upgrader = session.upgrade();
        auto upgrade_result = co_await upgrader().timeout(kOpTimeout);
        if (!upgrade_result) {
            co_await client.close();
            co_return false;
        }
        if (!upgrade_result.value()) {
            co_await client.close();
            co_return false;
        }

        // 5. 接收欢迎消息
        std::string welcome_msg;
        WsOpcode welcome_opcode;
        while (true) {
            auto recv_result = co_await session.getMessage(welcome_msg, welcome_opcode).timeout(kOpTimeout);
            if (!recv_result) {
                co_await client.close();
                co_return false;
            }
            if (recv_result.value()) {
                break;
            }
        }
        std::cout << "Received welcome message: " << welcome_msg << "\n";

        // 6. 发送和接收消息
        for (int i = 0; i < message_count; i++) {
            std::string msg = "Hello WSS #" + std::to_string(i + 1);

            // 发送文本消息
            while (true) {
                auto send_result = co_await session.sendText(msg);
                if (!send_result) {
                    co_await client.close();
                    co_return false;
                }
                if (send_result.value()) {
                    break;
                }
            }

            // 接收回显
            std::string echo_msg;
            WsOpcode echo_opcode;
            while (true) {
                auto recv_result = co_await session.getMessage(echo_msg, echo_opcode).timeout(kOpTimeout);
                if (!recv_result) {
                    co_await client.close();
                    co_return false;
                }
                if (recv_result.value()) {
                    break;
                }
            }
            std::cout << "Received echo: " << echo_msg << "\n";
        }

        // 7. 发送关闭帧
        while (true) {
            auto close_result = co_await session.sendClose(WsCloseCode::Normal);
            if (!close_result) {
                break;
            }
            if (close_result.value()) {
                break;
            }
        }

        co_await client.close();
        std::cout << "WSS client finished\n";
        co_return true;

    } catch (const std::exception& e) {
        co_return false;
    }
}

int main(int argc, char* argv[]) {
    std::string url = "wss://localhost:8443/ws";
    int message_count = 5;

    if (argc > 1) url = argv[1];
    if (argc > 2) message_count = std::atoi(argv[2]);

    std::cout << "========================================\n";
    std::cout << "WSS (WebSocket Secure) Client Example\n";
    std::cout << "========================================\n";
    std::cout << "URL: " << url << "\n";
    std::cout << "Messages: " << message_count << "\n";
    std::cout << "========================================\n";

    try {
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        runtime.start();

        auto join = runtime.spawn(wssClientTask(url, message_count));
        bool ok = false;
        if (join) {
            auto result = join->join();
            ok = result && result.value();
        }
        runtime.stop();
        std::cout << "Done.\n";

        if (!ok) {
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
