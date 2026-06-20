#include "common/example_common.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

import galay.http;
import galay.websocket;

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

Task<void> handleWebSocketConnection(WsConn& ws_conn) {
    WsReaderSetting reader_setting;
    reader_setting.max_frame_size = 1024 * 1024;
    reader_setting.max_message_size = 10 * 1024 * 1024;

    auto reader = ws_conn.getReader(reader_setting);
    auto writer = ws_conn.getWriter(WsWriterSetting::byServer());

    auto welcome_result = co_await writer.sendText("Welcome to import WebSocket server!");
    if (!welcome_result) {
        co_return;
    }

    while (true) {
        std::string message;
        WsOpcode opcode{};
        auto result = co_await reader.getMessage(message, opcode);
        if (!result) {
            break;
        }
        if (!result.value()) {
            continue;
        }

        if (opcode == WsOpcode::Ping) {
            auto pong_result = co_await writer.sendPong(message);
            if (!pong_result) {
                break;
            }
            continue;
        }

        if (opcode == WsOpcode::Close) {
            (void)co_await writer.sendClose();
            break;
        }

        if (opcode == WsOpcode::Text || opcode == WsOpcode::Binary) {
            auto echo_result = co_await writer.sendText("Echo: " + message);
            if (!echo_result) {
                break;
            }
        }
    }

    (void)co_await ws_conn.close();
    co_return;
}

Task<void> handleHttpRequest(HttpConn conn) {
    auto reader = conn.getReader();
    HttpRequest request;
    auto read_result = co_await reader.getRequest(request);
    if (!read_result) {
        (void)co_await conn.close();
        co_return;
    }

    if (request.header().uri() == "/ws") {
        auto upgrade_result = WsUpgrade::handleUpgrade(request);
        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(upgrade_result.response);
        if (!send_result || !upgrade_result.success) {
            (void)co_await conn.close();
            co_return;
        }

        WsConn ws_conn = WsConn::from(std::move(conn), true);
        co_await handleWebSocketConnection(ws_conn);
        co_return;
    }

    auto response = Http1_1ResponseBuilder::ok()
        .html(
            "<html><body>"
            "<h1>Import WebSocket Server</h1>"
            "<p>Connect to <code>ws://127.0.0.1:8080/ws</code>.</p>"
            "</body></html>")
        .build();
    auto writer = conn.getWriter();
    (void)co_await writer.sendResponse(response);
    (void)co_await conn.close();
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = galay::http::example::kDefaultWsEchoPort;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    try {
        HttpServer server(HttpServerBuilder().host("0.0.0.0").port(port).build());
        std::cout << "Import WebSocket server: ws://127.0.0.1:" << port << "/ws\n";
        server.start(handleHttpRequest);
        while (server.isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
