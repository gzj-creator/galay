/**
 * @file B5-Websocket.cc
 * @brief WebSocket 服务器压测程序
 * @details 配合 B4-WebsocketClient 进行 WebSocket 性能测试
 * @usage benchmark_ws_ws_server_throughput [port] [io_threads] [nodelay:on|off]
 */

#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-http/server/http_server.h>
#include <galay/cpp/galay-ws/server/ws_upgrade.h>
#include <galay/cpp/galay-ws/kernel/ws_conn.h>
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-http/protoc/http_response.h>
#include <galay/cpp/galay-http/builder/http_builder.h>
#include <galay/cpp/galay-ws/kernel/writer_cfg.h>
#include "benchmark/cpp/ws/ws_benchmark_args.h"
#include <iostream>
#include <atomic>
#include <signal.h>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <thread>
#include <chrono>
using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

// 统计信息
std::atomic<int> total_connections{0};
std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

/**
 * @brief WebSocket 连接处理协程
 * @param ws_conn WebSocket 连接
 * @return 执行该操作的协程任务，完成后无结果值
 */
Task<void> handle_web_socket_connection(WsConn& ws_conn) {
    int conn_id = total_connections.fetch_add(1);

    auto reader = ws_conn.get_reader();
    auto writer = ws_conn.get_writer(WsWriterSetting::by_server());

    // 发送欢迎消息
    auto res = co_await writer.send_text("Welcome to WebSocket Benchmark Server!");
    if(!res) {
        co_return;
    }

    // 消息循环
    std::string message;
    WsOpcode opcode = WsOpcode::Text;
    while (true) {
        message.clear();

        auto result = co_await ws_conn.echo_once(message, opcode);

        if (!result) {
            // 连接错误
            break;
        }

        if (!result.value()) {
            // 消息未完成，继续读取
            continue;
        }

        // 处理不同类型的消息
        if (opcode == WsOpcode::Text || opcode == WsOpcode::Binary) {

        } else if (opcode == WsOpcode::Ping) {
            // 响应 Ping
            auto pong_res = co_await writer.send_pong(message);
            if (!pong_res) {
                goto cleanup;
            }

        } else if (opcode == WsOpcode::Close) {
            // 客户端关闭连接
            auto close_res = co_await writer.send_close();
            if (!close_res) {
            } else {
            }
            break;
        }
    }

cleanup:
    co_await ws_conn.close();
    co_return;
}

/**
 * @brief HTTP 请求处理协程
 * @param conn 连接对象
 * @return 执行该操作的协程任务，完成后无结果值
 */
Task<void> handle_http_request(HttpConn conn) {
    static std::atomic<int> req_id{0};
    int current_req_id = req_id.fetch_add(1);


    auto reader = conn.get_reader();
    HttpRequest request;

    auto read_result = co_await reader.get_request(request);
    if (!read_result) {
        co_await conn.close();
        co_return;
    }


    // 检查是否是 WebSocket 升级请求
    if (request.header().uri() == "/ws" || request.header().uri() == "/") {
        auto upgrade_result = WsUpgrade::handle_upgrade(request);

        if (!upgrade_result.success) {
            auto writer = conn.get_writer();
            auto result = co_await writer.send_response(upgrade_result.response);
            if (!result) {
            }
            co_await conn.close();
            co_return;
        }

        // 发送升级响应
        auto writer = conn.get_writer();
        auto send_result = co_await writer.send_response(upgrade_result.response);

        if (!send_result) {
            co_await conn.close();
            co_return;
        }

        WsConn ws_conn = WsConn::from(std::move(conn), true);

        co_await handle_web_socket_connection(ws_conn);
    } else {
        // 非 WebSocket 请求，返回 404
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::NotFound_404)
            .body("Not Found")
            .build_move();

        auto writer = conn.get_writer();
        auto result = co_await writer.send_response(response);
        if (!result) {
        }
        co_await conn.close();
    }

    co_return;
}

int main(int argc, char* argv[]) {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    // 压测默认关闭日志，避免日志 IO 成为吞吐瓶颈。
    // 设置 GALAY_HTTP_BENCH_LOG=1 可开启文件日志。
    const char* bench_log = std::getenv("GALAY_HTTP_BENCH_LOG");
    if (bench_log != nullptr && std::string_view(bench_log) == "1") {
    } else {
    }

    uint16_t port = 8080;
    int io_threads = 4;
    if (argc >= 2) {
        port = std::atoi(argv[1]);
    }
    if (argc >= 3) {
        io_threads = std::atoi(argv[2]);
    }
    const bool tcp_no_delay = galay::benchmark::ws::resolve_benchmark_server_no_delay(argc, argv, 3);

    std::cout << "========================================" << std::endl;
    std::cout << "WebSocket Benchmark Server" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "IO Threads: " << io_threads << std::endl;
    std::cout << "TCP_NODELAY: " << (tcp_no_delay ? "on" : "off") << std::endl;
    std::cout << "Configured Compute Threads: 0" << std::endl;
    std::cout << "WebSocket endpoint: ws://localhost:" << port << "/ws" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        HttpServer server(HttpServerBuilder<>()
            .host("0.0.0.0")
            .port(port)
            .io_scheduler_count(static_cast<size_t>(io_threads))
            .parallel_scheduler_count(0)
            .tcp_no_delay(tcp_no_delay)
            .build_config());


        server.start(handle_http_request);

        std::cout << "Server started successfully!\n" << std::endl;
        std::cout << "Runtime Config: io=" << server.get_runtime().get_io_scheduler_count()
                  << " parallel=" << server.get_runtime().get_parallel_scheduler_count()
                  << " (configured io=" << io_threads << " parallel=0)" << std::endl;

        // 等待停止信号
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nShutting down..." << std::endl;
        server.stop();
        std::cout << "Server stopped." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
