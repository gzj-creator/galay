/**
 * @file b7_wss.cc
 * @brief WSS (WebSocket Secure) 服务器压测程序（纯净版）
 * @details 配合 B8-WssClient 进行 WSS 性能测试
 *          移除统计功能，由客户端负责统计
 */

#include "../common/benchmark_environment.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <galay/cpp/galay-http/server/http_server.h>
#include <galay/cpp/galay-ws/kernel/ws_conn.h>
#include <galay/cpp/galay-ws/server/ws_upgrade.h>
#include <galay/cpp/galay-ws/kernel/writer_cfg.h>
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-http/builder/http_builder.h>
#include "benchmark/cpp/ws/ws_benchmark_args.h"

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

static volatile bool g_running = true;

void signal_handler(int) {
    g_running = false;
}

Task<void> handle_wss_connection(WssConn& ws_conn) {

    auto writer = ws_conn.get_writer(WsWriterSetting::by_server());

    auto welcome_result = co_await writer.send_text("Welcome to WSS Benchmark Server!");
    if (!welcome_result) {
        co_return;
    }

    auto echo_result = co_await ws_conn.echo_loop_consume();
    if (!echo_result) {
    }

    co_await ws_conn.close();
    co_return;
}

/**
 * @brief HTTPS 请求处理器（处理 WSS 升级）
 */
Task<void> https_handler(HttpConnImpl<galay::ssl::SslSocket> conn) {
    auto reader = conn.get_reader();
    HttpRequest request;

    // 读取请求
    while (true) {
        auto r = co_await reader.get_request(request);
        if (!r) {
            co_await conn.close();
            co_return;
        }
        if (r.value()) break;
    }


    // 检查是否是 WebSocket 升级请求
    std::string uri = request.header().uri();
    if (uri == "/ws" || uri.starts_with("/ws?") || uri == "/") {
        auto upgrade_result = WsUpgrade::handle_upgrade(request);

        if (!upgrade_result.success) {
            auto writer = conn.get_writer();
            auto result = co_await writer.send_response(upgrade_result.response);
            if (!result) {
            }
            co_await conn.close();
            co_return;
        }


        // 发送 101 Switching Protocols
        auto writer = conn.get_writer();
        auto r = co_await writer.send_response(upgrade_result.response);
        if (!r) {
            co_await conn.close();
            co_return;
        }

        WssConn ws_conn = WssConn::from(std::move(conn), true);
        co_await handle_wss_connection(ws_conn);
        co_return;
    }

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
    co_return;
}

int main(int argc, char* argv[]) {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    bool debug_log = false;
    if (const char* env = std::getenv("GALAY_WSS_DEBUG_LOG")) {
        debug_log = std::atoi(env) != 0;
    }
    if (debug_log) {
    } else {
    }

    int port = 8443;
    int io_threads = 4;
    std::string cert_path = "../cert/test.crt";
    std::string key_path = "../cert/test.key";

    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) io_threads = std::atoi(argv[2]);
    if (argc > 3) cert_path = argv[3];
    if (argc > 4) key_path = argv[4];
    const bool tcp_no_delay = galay::benchmark::ws::resolve_benchmark_server_no_delay(argc, argv, 5);

    std::cout << "========================================\n";
    std::cout << "WSS (WebSocket Secure) Benchmark Server\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "TCP_NODELAY: " << (tcp_no_delay ? "on" : "off") << "\n";
    std::cout << "Configured Compute Threads: 0\n";
    std::cout << "Cert: " << cert_path << "\n";
    std::cout << "Key:  " << key_path << "\n";
    std::cout << "WSS endpoint: wss://localhost:" << port << "/ws\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        HttpsServer server(HttpsServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .cert_path(cert_path)
            .key_path(key_path)
            .io_scheduler_count(static_cast<size_t>(io_threads))
            .parallel_scheduler_count(0)
            .tcp_no_delay(tcp_no_delay)
            .build());

        server.start(https_handler);

        std::cout << "Server started successfully!\n";
        std::cout << "Runtime Config: io=" << server.get_runtime().get_io_scheduler_count()
                  << " parallel=" << server.get_runtime().get_parallel_scheduler_count()
                  << " (configured io=" << io_threads << " parallel=0)\n";
        std::cout << "Waiting for requests...\n\n";

        // 等待停止信号
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nShutting down...\n";
        server.stop();
        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#else

int main() {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
