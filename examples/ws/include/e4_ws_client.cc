/**
 * @file E4-WebsocketClient.cc
 * @brief WebSocket 客户端示例
 * @details 展示如何使用 WsClient 连接到 WebSocket 服务器并进行双向通信
 */

#include <iostream>
#include "galay-ws/client/ws_client.h"
#include "galay-ws/kernel/writer_cfg.h"
#include "galay-kernel/core/runtime.h"

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

/**
 * @brief WebSocket 客户端协程
 */
Task<bool> runWebSocketClient(const std::string& url) {

    // 1. 创建 WsClient
    auto client = WsClientBuilder().build();

    // 2. 连接到服务器
    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        co_return false;
    }

    // 3. 获取 Session
    WsReaderSetting reader_setting;
    reader_setting.max_frame_size = 1024 * 1024;  // 1MB
    reader_setting.max_message_size = 10 * 1024 * 1024;  // 10MB

    auto session_result = client.getSession(WsWriterSetting::byClient(), 8192, reader_setting);
    if (!session_result) {
        co_return false;
    }
    auto& session = *session_result.value();

    // 4. 执行 WebSocket 升级
    auto upgrader = session.upgrade();
    auto result = co_await upgrader();
    if (!result) {
        co_return false;
    }
    if (!result.value()) {
        co_return false;
    }

    // 5. 获取 Reader 和 Writer
    auto reader = session.getReader();
    auto writer = session.getWriter();

    // 6. 读取欢迎消息
    std::string welcome_message;
    WsOpcode welcome_opcode;

    while (true) {
        auto result = co_await reader.getMessage(welcome_message, welcome_opcode);
        if (!result) {
            co_await client.close();
            co_return false;
        }
        if (result.value()) {
            break;  // 消息接收完成
        }
    }
    std::cout << "Received welcome message: " << welcome_message << "\n";

    // 7. 发送测试消息
    std::vector<std::string> test_messages = {
        "Hello from WebSocket client!",
        "This is message number 2",
        "Testing WebSocket communication",
        "Final test message"
    };

    for (const auto& msg : test_messages) {
        // 发送消息
        while (true) {
            auto send_result = co_await writer.sendText(msg);
            if (!send_result) {
                co_await client.close();
                co_return false;
            }
            if (send_result.value()) {
                break;
            }
        }

        // 接收回显消息
        std::string echo_message;
        WsOpcode echo_opcode;

        while (true) {
            auto result = co_await reader.getMessage(echo_message, echo_opcode);
            if (!result) {
                if (result.error().code() == kWsConnectionClosed) {
                    co_await client.close();
                    co_return false;
                }
                co_await client.close();
                co_return false;
            }

            if (!result.value()) {
                continue;  // 消息不完整，继续读取
            }

            // 处理不同类型的消息
            if (echo_opcode == WsOpcode::Ping) {
                while (true) {
                    auto pong_result = co_await writer.sendPong(echo_message);
                    if (!pong_result) {
                        co_await client.close();
                        co_return false;
                    }
                    if (pong_result.value()) {
                        break;
                    }
                }
                continue;
            }
            else if (echo_opcode == WsOpcode::Pong) {
                continue;
            }
            else if (echo_opcode == WsOpcode::Close) {
                while (true) {
                    auto close_result = co_await writer.sendClose();
                    if (!close_result || close_result.value()) {
                        break;
                    }
                }
                co_await client.close();
                co_return false;
            }
            else if (echo_opcode == WsOpcode::Text || echo_opcode == WsOpcode::Binary) {
                break;  // 收到数据消息
            }
        }

        std::cout << "Received echo: " << echo_message << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 8. 发送 Ping 测试
    while (true) {
        auto ping_result = co_await writer.sendPing("ping");
        if (!ping_result) {
            co_await client.close();
            co_return false;
        }
        if (ping_result.value()) {
            break;
        }
    }

    // 等待 Pong 响应
    std::string pong_message;
    WsOpcode pong_opcode;
    while (true) {
        auto result = co_await reader.getMessage(pong_message, pong_opcode);
        if (!result) {
            co_await client.close();
            co_return false;
        }
        if (!result.value()) {
            continue;
        }
        if (pong_opcode == WsOpcode::Pong) {
            std::cout << "Received Pong response\n";
            break;
        }
    }

    // 9. 关闭连接
    while (true) {
        auto close_result = co_await writer.sendClose();
        if (!close_result || close_result.value()) {
            break;
        }
    }
    co_await client.close();
    std::cout << "WebSocket client finished\n";
    co_return true;
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    std::string url = "ws://127.0.0.1:8080/ws";

    if (argc > 1) {
        url = argv[1];
    }

    std::cout << "========================================\n";
    std::cout << "WebSocket Client Example\n";
    std::cout << "========================================\n";
    std::cout << "URL: " << url << "\n";
    std::cout << "========================================\n\n";

    try {
        // 创建 Runtime
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        runtime.start();


        auto join = runtime.spawn(runWebSocketClient(url));
        bool ok = false;
        if (join) {
            auto result = join->join();
            ok = result && result.value();
        }

        // 停止 Runtime
        runtime.stop();

        if (!ok) {
            return 1;
        }

    } catch (const std::exception& e) {
        return 1;
    }

    return 0;
}
