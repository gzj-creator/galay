#include "galay-kernel/core/runtime.h"

#include <iostream>
#include <string>

import galay.websocket;

using namespace galay::websocket;
using namespace galay::kernel;

Task<bool> runWebSocketClient(const std::string& url) {
    auto client = WsClientBuilder().build();
    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << "\n";
        co_return false;
    }

    WsReaderSetting reader_setting;
    reader_setting.max_frame_size = 1024 * 1024;
    reader_setting.max_message_size = 10 * 1024 * 1024;

    auto session_result = client.getSession(WsWriterSetting::byClient(), 8192, reader_setting);
    if (!session_result) {
        co_return false;
    }
    auto& session = *session_result.value();
    auto upgrade_result = co_await session.upgrade()();
    if (!upgrade_result || !upgrade_result.value()) {
        std::cerr << "Upgrade failed\n";
        co_return false;
    }

    auto reader = session.getReader();
    auto writer = session.getWriter();

    std::string message;
    WsOpcode opcode{};
    while (true) {
        auto result = co_await reader.getMessage(message, opcode);
        if (!result) {
            std::cerr << "Failed to receive welcome message: " << result.error().message() << "\n";
            co_return false;
        }
        if (result.value()) {
            break;
        }
    }
    std::cout << "Welcome: " << message << "\n";

    auto send_result = co_await writer.sendText("Hello from import websocket client!");
    if (!send_result) {
        std::cerr << "Send failed: " << send_result.error().message() << "\n";
        co_return false;
    }

    while (true) {
        auto result = co_await reader.getMessage(message, opcode);
        if (!result) {
            std::cerr << "Failed to read echo: " << result.error().message() << "\n";
            co_return false;
        }
        if (!result.value()) {
            continue;
        }
        if (opcode == WsOpcode::Text || opcode == WsOpcode::Binary) {
            break;
        }
        if (opcode == WsOpcode::Ping) {
            auto pong_result = co_await writer.sendPong(message);
            if (!pong_result) {
                std::cerr << "Pong failed: " << pong_result.error().message() << "\n";
                co_return false;
            }
        }
    }

    std::cout << "Echo: " << message << "\n";
    (void)co_await writer.sendClose();
    (void)co_await client.close();
    co_return true;
}

int main(int argc, char* argv[]) {
    std::string url = "ws://127.0.0.1:8080/ws";
    if (argc > 1) {
        url = argv[1];
    }

    try {
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        runtime.start();
        auto join = runtime.spawn(runWebSocketClient(url));
        bool ok = false;
        if (join) {
            auto result = join->join();
            ok = result && result.value();
        }
        runtime.stop();
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Client error: " << e.what() << "\n";
        return 1;
    }
}
