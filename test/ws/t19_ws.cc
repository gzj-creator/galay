/**
 * @file T19-WsClient.cc
 * @brief WebSocket 客户端测试程序
 * @details 用于测试 WebSocket 客户端功能
 *
 * 使用方法:
 *   ./test/T19-WsClient <host> <port> [messages]
 *   默认: localhost 8080 5
 */

#include "galay-ws/client/ws_client.h"
#include "galay-kernel/core/runtime.h"
#include "galay-ws/kernel/reader_cfg.h"
#include "galay-ws/kernel/writer_cfg.h"
#include <iostream>
#include <atomic>

using namespace galay::websocket;
using namespace galay::kernel;

std::atomic<int> success_count{0};
std::atomic<int> fail_count{0};

/**
 * @brief 客户端测试协程
 */
Task<void> testClient(const std::string& host, uint16_t port, int num_messages) {
    auto client = WsClientBuilder().build();

    // 构建 WebSocket URL
    std::string url = "ws://" + host + ":" + std::to_string(port) + "/ws";
    std::cout << "Connecting to " << url << "..." << std::endl;

    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << std::endl;
        fail_count++;
        co_return;
    }
    std::cout << "Connected!" << std::endl;
    auto session_result = client.getSession(WsWriterSetting::byClient());
    if (!session_result) {
        co_await client.close();
        co_return;
    }
    auto& session = *session_result.value();
    // Upgrade to WebSocket
    std::cout << "Upgrading to WebSocket..." << std::endl;
    auto upgrader = session.upgrade();
    auto upgrade_result = co_await upgrader();
    if (!upgrade_result) {
        std::cerr << "Upgrade failed: " << upgrade_result.error().message() << std::endl;
        fail_count++;
        co_return;
    }
    if (!upgrade_result.value()) {
        std::cerr << "Upgrade failed: incomplete result" << std::endl;
        fail_count++;
        co_return;
    }
    std::cout << "Upgrade successful!" << std::endl;

    auto& reader = session.getReader();
    auto& writer = session.getWriter();

    // 接收欢迎消息
    std::cout << "Waiting for welcome message..." << std::endl;
    std::string welcome_msg;
    WsOpcode welcome_opcode;
    bool welcome_received = false;
    while (!welcome_received) {
        auto welcome_result = co_await reader.getMessage(welcome_msg, welcome_opcode);
        if (!welcome_result.has_value()) {
            std::cerr << "Failed to receive welcome: " << welcome_result.error().message() << std::endl;
            fail_count++;
            co_return;
        }
        if (welcome_result.value()) {
            welcome_received = true;
            std::cout << "Welcome message: " << welcome_msg << std::endl;
        }
    }

    // 发送多个文本消息
    std::cout << "Sending " << num_messages << " messages..." << std::endl;
    for (int i = 0; i < num_messages; i++) {
        std::string message = "Test message #" + std::to_string(i + 1);

        // 发送消息
        std::cout << "Sending: " << message << std::endl;
        auto send_result = co_await writer.sendText(message);
        if (!send_result) {
            std::cerr << "Failed to send message: " << send_result.error().message() << std::endl;
            fail_count++;
            break;
        }

        // 接收回显响应
        std::string response;
        WsOpcode opcode;
        bool response_received = false;
        while (!response_received) {
            auto recv_result = co_await reader.getMessage(response, opcode);
            if (!recv_result.has_value()) {
                std::cerr << "Failed to read response: " << recv_result.error().message() << std::endl;
                fail_count++;
                break;
            }

            if (recv_result.value()) {
                response_received = true;
                if (opcode == WsOpcode::Text) {
                    std::cout << "Received: " << response << std::endl;
                    success_count++;
                } else {
                    std::cerr << "Unexpected opcode: " << static_cast<int>(opcode) << std::endl;
                    fail_count++;
                }
            }
        }

        if (!response_received) {
            break;
        }
    }

    // 发送 Ping
    std::cout << "Sending Ping..." << std::endl;
    auto ping_result = co_await writer.sendPing("ping");
    if (!ping_result) {
        std::cerr << "Failed to send ping: " << ping_result.error().message() << std::endl;
    } else {
        std::cout << "Ping sent" << std::endl;

        // 等待 Pong
        std::string pong_data;
        WsOpcode pong_opcode;
        bool pong_received = false;
        while (!pong_received) {
            auto pong_result = co_await reader.getMessage(pong_data, pong_opcode);
            if (!pong_result.has_value()) {
                break;
            }
            if (pong_result.value()) {
                pong_received = true;
                if (pong_opcode == WsOpcode::Pong) {
                    std::cout << "Pong received" << std::endl;
                }
            }
        }
    }

    // 关闭连接
    std::cout << "Closing connection..." << std::endl;
    co_await writer.sendClose();
    std::cout << "Connection closed." << std::endl;

    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 8080;
    int num_messages = 5;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    if (argc > 3) num_messages = std::atoi(argv[3]);

    std::cout << "========================================\n";
    std::cout << "WebSocket Client Test\n";
    std::cout << "========================================\n";
    std::cout << "Target: " << host << ":" << port << "\n";
    std::cout << "Messages: " << num_messages << "\n";
    std::cout << "========================================\n\n";

    try {
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        scheduleTask(scheduler, testClient(host, port, num_messages));

        // Wait for completion (max 30 seconds)
        for (int i = 0; i < 300; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (success_count.load() + fail_count.load() >= num_messages) {
                break;
            }
        }

        runtime.stop();

        std::cout << "\n========================================\n";
        std::cout << "Test Results:\n";
        std::cout << "========================================\n";
        std::cout << "Success: " << success_count << "\n";
        std::cout << "Failed: " << fail_count << "\n";
        std::cout << "Total: " << (success_count + fail_count) << "/" << num_messages << "\n";
        std::cout << "========================================\n";

        return (fail_count > 0) ? 1 : 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
