/**
 * @file example_websocket_server.cc
 * @brief WebSocket 服务器完整示例
 * @details 展示如何使用 WsUpgrade 和手动处理控制帧（Ping/Pong/Close）
 */

#include <chrono>
#include "galay-http/server/http_server.h"
#include "galay-ws/server/ws_upgrade.h"
#include "galay-ws/kernel/ws_conn.h"
#include "galay-ws/kernel/writer_cfg.h"
#include "galay-http/protoc/http_request.h"
#include "galay-http/protoc/http_response.h"
#include "galay-http/builder/http_builder.h"

#ifdef USE_KQUEUE
#include "galay-kernel/core/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include "galay-kernel/core/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include "galay-kernel/core/io_uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;
using namespace std::chrono_literals;

// ==================== WebSocket 处理器 ====================

/**
 * @brief 处理 WebSocket 连接
 * @param ws_conn WebSocket 连接（通过引用传递）
 */
Task<void> handleWebSocketConnection(WsConn& ws_conn) {


     // 升级到 WebSocket 连接
     WsReaderSetting reader_setting;
     reader_setting.max_frame_size = 1024 * 1024;  // 1MB
     reader_setting.max_message_size = 10 * 1024 * 1024;  // 10MB

    // 获取 Reader 和 Writer（必须在协程开始时获取，保证 ws_conn 生命周期）
    auto reader = ws_conn.getReader(reader_setting);
    auto writer = ws_conn.getWriter(WsWriterSetting::byServer());

    // 发送欢迎消息
    auto send_result = co_await writer.sendText("Welcome to WebSocket server!");
    if (!send_result) {
        co_return;
    }

    // 消息循环
    while (true) {
        std::string message;
        WsOpcode opcode;

        // 读取消息（包括数据帧和控制帧）
        auto result = co_await reader.getMessage(message, opcode).timeout(1000ms);

        if (!result.has_value()) {
            WsError error = result.error();
            if (error.code() == kWsConnectionClosed) {
                break;
            }
            break;
        }

        if (!result.value()) {
            // 消息不完整，继续读取
            continue;
        }

        // 根据 opcode 判断消息类型并处理
        if (opcode == WsOpcode::Ping) {
            // 收到 Ping，发送 Pong 响应
            auto pong_result = co_await writer.sendPong(message);
            if (!pong_result) {
                break;
            }
        }
        else if (opcode == WsOpcode::Pong) {
            // 收到 Pong 响应
        }
        else if (opcode == WsOpcode::Close) {
            // 收到关闭请求
            auto close_result = co_await writer.sendClose();
            if (!close_result) {
            }
            break;
        }
        else if (opcode == WsOpcode::Text || opcode == WsOpcode::Binary) {
            // 处理数据消息

            // 回显消息
            std::string echo_msg = "Echo: " + message;
            auto echo_result = co_await writer.sendText(echo_msg);
            if (!echo_result) {
                break;
            }
        }
    }

    // 关闭连接
    auto close_result = co_await ws_conn.close();
    if (!close_result) {
    }
    co_return;
}

/**
 * @brief HTTP 请求处理器（处理 WebSocket 升级）
 * @param conn HTTP 连接
 */
Task<void> handleHttpRequest(HttpConn conn) {
    // 读取 HTTP 请求
    auto reader = conn.getReader();
    HttpRequest request;

    auto read_result = co_await reader.getRequest(request);
    if (!read_result) {
        auto close_result = co_await conn.close();
        if (!close_result) {
        }
        co_return;
    }


    // 检查是否是 WebSocket 升级请求
    if (request.header().uri() == "/ws") {
        // 处理 WebSocket 升级
        auto upgrade_result = WsUpgrade::handleUpgrade(request);

        if (!upgrade_result.success) {

            // 发送错误响应
            auto writer = conn.getWriter();
            auto result = co_await writer.sendResponse(upgrade_result.response);
            if (!result) {
            }
            auto close_result = co_await conn.close();
            if (!close_result) {
            }
            co_return;
        }


        // 发送 101 Switching Protocols 响应
        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(upgrade_result.response);

        if (!send_result) {
            auto close_result = co_await conn.close();
            if (!close_result) {
            }
            co_return;
        }

        // 从 HttpConn 创建 WebSocket 连接（转移所有权）
        WsConn ws_conn = WsConn::from(std::move(conn), true);

        // 处理 WebSocket 连接（通过引用传递，避免移动导致引用失效）
        co_await handleWebSocketConnection(ws_conn);
        co_return;
    }

    // 普通 HTTP 请求
    auto response = Http1_1ResponseBuilder()
        .status(HttpStatusCode::OK_200)
        .header("Content-Type", "text/html; charset=utf-8")
        .buildMove();

    std::string body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>WebSocket Example</title>
</head>
<body>
    <h1>WebSocket Server Example</h1>
    <p>Connect to WebSocket endpoint: <code>ws://localhost:8080/ws</code></p>

    <h2>Test with JavaScript:</h2>
    <pre>
const ws = new WebSocket('ws://localhost:8080/ws');

ws.onopen = () => {
    console.log('Connected');
    ws.send('Hello Server!');
};

ws.onmessage = (event) => {
    console.log('Received:', event.data);
};

ws.onerror = (error) => {
    console.error('Error:', error);
};

ws.onclose = () => {
    console.log('Disconnected');
};
    </pre>
</body>
</html>)";

    response.header().headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));
    response.setBodyStr(std::move(body));

    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);
    if (!result) {
    }
    auto close_result = co_await conn.close();
    if (!close_result) {
    }
    co_return;
}

// ==================== 主函数 ====================

int main() {

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    // 配置服务器
    // 创建 HTTP 服务器
    HttpServer server(HttpServerBuilder()
        .host("0.0.0.0")
        .port(8080)
        .backlog(128)
        .ioSchedulerCount(4)
        .computeSchedulerCount(2)
        .build());

    // 启动服务器

    // 启动服务器并传入处理器
    server.start(handleHttpRequest);

    // 保持服务器运行
    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
#else
    return 1;
#endif
}
