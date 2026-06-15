/**
 * @file T18-WsServer.cc
 * @brief WebSocket 服务器测试程序
 * @details 基于 HttpServer 实现 WebSocket 服务器测试
 *
 * 使用方法:
 *   ./test/T18-WsServer [port]
 *   默认端口: 8080
 */

#include "galay-http/server/http_server.h"
#include "galay-ws/server/ws_upgrade.h"
#include "galay-ws/kernel/ws_conn.h"
#include "galay-http/protoc/http_request.h"
#include "galay-http/protoc/http_response.h"
#include "galay-http/builder/http_builder.h"
#include "galay-ws/kernel/writer_cfg.h"
#include <iostream>
#include <atomic>
#include <csignal>
#include <utility>

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_message_count{0};
static std::atomic<uint64_t> g_connection_count{0};

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief 处理 WebSocket 连接
 */
Task<void> handleWebSocketConnection(WsConn& ws_conn) {
    g_connection_count++;

    auto reader = ws_conn.getReader();
    auto writer = ws_conn.getWriter(WsWriterSetting::byServer());

    // 发送欢迎消息
    auto send_result = co_await writer.sendText("Welcome to WebSocket Test Server!");
    if (!send_result) {
        co_return;
    }

    // 消息循环
    while (true) {
        std::string message;
        WsOpcode opcode;

        // 循环等待接收消息
        bool recv_success = false;
        while (true) {
            auto result = co_await reader.getMessage(message, opcode);

            if (!result.has_value()) {
                WsError error = result.error();
                if (error.code() == kWsConnectionClosed) {
                    co_await ws_conn.close();
                    co_return;
                }
                co_await ws_conn.close();
                co_return;
            }

            if (result.value()) {
                recv_success = true;
                break;
            }
        }

        if (!recv_success) {
            break;
        }

        g_message_count++;

        // 处理不同类型的消息
        if (opcode == WsOpcode::Ping) {
            auto pong_result = co_await writer.sendPong(message);
            if (!pong_result) {
                co_await ws_conn.close();
                co_return;
            }
        }
        else if (opcode == WsOpcode::Pong) {
        }
        else if (opcode == WsOpcode::Close) {
            auto close_result = co_await writer.sendClose();
            if (!close_result) {
            }
            break;
        }
        else if (opcode == WsOpcode::Text) {

            // Echo back
            std::string echo_msg = "Echo: " + message;
            auto echo_result = co_await writer.sendText(echo_msg);
            if (!echo_result) {
                co_await ws_conn.close();
                co_return;
            }
        }
        else if (opcode == WsOpcode::Binary) {

            // Echo back binary
            auto echo_result = co_await writer.sendBinary(message);
            if (!echo_result) {
                co_await ws_conn.close();
                co_return;
            }
        }
    }

    co_await ws_conn.close();
    co_return;
}

/**
 * @brief HTTP 请求处理器
 */
Task<void> handleHttpRequest(HttpConn conn) {
    auto reader = conn.getReader();
    HttpRequest request;

    auto read_result = co_await reader.getRequest(request);
    if (!read_result) {
        co_await conn.close();
        co_return;
    }


    // 检查是否是 WebSocket 升级请求
    if (request.header().uri() == "/ws" || request.header().uri() == "/") {
        auto upgrade_result = WsUpgrade::handleUpgrade(request);

        if (!upgrade_result.success) {

            auto writer = conn.getWriter();
            co_await writer.sendResponse(upgrade_result.response);
            co_await conn.close();
            co_return;
        }


        // 发送升级响应
        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(upgrade_result.response);

        if (!send_result) {
            co_await conn.close();
            co_return;
        }

        WsConn ws_conn = WsConn::from(std::move(conn), true);

        co_await handleWebSocketConnection(ws_conn);
    }
    else {
        // 普通 HTTP 请求
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::OK_200)
            .header("Content-Type", "text/html")
            .buildMove();

        std::string body = "<html><body><h1>WebSocket Test Server</h1><p>Connect to /ws for WebSocket</p></body></html>";
        response.setBodyStr(std::move(body));

        auto writer = conn.getWriter();
        co_await writer.sendResponse(response);
        co_await conn.close();
    }

    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    std::cout << "========================================\n";
    std::cout << "WebSocket Server Test\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "WebSocket endpoint: ws://localhost:" << port << "/ws\n";
    std::cout << "Test command: ./test/T19-WsClient localhost " << port << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpServer server(HttpServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .ioSchedulerCount(4)
            .computeSchedulerCount(0)
            .build());


        server.start(handleHttpRequest);

        std::cout << "Server started successfully!\n\n";

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\n\nShutting down...\n";
        std::cout << "Total connections: " << g_connection_count << "\n";
        std::cout << "Total messages: " << g_message_count << "\n";

        server.stop();
        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
