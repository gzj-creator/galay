/**
 * @file b14_https.cc
 * @brief HTTPS 服务器压测程序（纯净版）
 * @details 提供 keep-alive 的 200 OK 文本响应，用于与 Go/Rust HTTPS 服务横向对比
 */

#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-http/server/http_server.h>
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::http;
using namespace galay::kernel;

static volatile bool g_running = true;
static constexpr std::string_view kPlainTextOkResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: 2\r\n"
    "\r\n"
    "OK";

void signal_handler(int) {
    g_running = false;
}

Task<void> handle_https_request(HttpConnImpl<galay::ssl::SslSocket> conn) {
    auto reader = conn.get_reader();
    auto writer = conn.get_writer();

    while (true) {
        HttpRequest request;

        while (true) {
            auto read_result = co_await reader.get_request(request);
            if (!read_result) {
                co_return;
            }
            if (read_result.value()) {
                break;
            }
        }

        auto send_result = co_await writer.send_view(kPlainTextOkResponse);
        if (!send_result) {
            co_return;
        }
    }
}

int main(int argc, char* argv[]) {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }


    uint16_t port = 9444;
    int io_threads = 4;
    std::string cert_path = "cert/test.crt";
    std::string key_path = "cert/test.key";

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        io_threads = std::atoi(argv[2]);
    }
    if (argc > 3) {
        cert_path = argv[3];
    }
    if (argc > 4) {
        key_path = argv[4];
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "========================================\n";
    std::cout << "HTTPS Server Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "Cert: " << cert_path << "\n";
    std::cout << "Key:  " << key_path << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    try {
        HttpsServer server(HttpsServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .cert_path(cert_path)
            .key_path(key_path)
            .io_scheduler_count(static_cast<size_t>(io_threads))
            .build());

        server.start(handle_https_request);
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        server.stop();
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
