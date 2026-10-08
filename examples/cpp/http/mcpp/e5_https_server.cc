#include "common/example_common.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

import galay.http;

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::http;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

Task<void> https_handler(HttpConnImpl<galay::ssl::SslSocket> conn) {
    auto reader = conn.get_reader();
    auto writer = conn.get_writer();

    while (true) {
        HttpRequest request;
        while (true) {
            auto read_result = co_await reader.get_request(request);
            if (!read_result) {
                (void)co_await conn.close();
                co_return;
            }
            if (read_result.value()) {
                break;
            }
        }

        const bool keep_alive =
            request.header().is_keep_alive() && !request.header().is_connection_close();
        const std::string request_body = request.get_body_str();
        const std::string response_body = request_body.empty()
            ? "Echo: (empty body)"
            : "Echo: " + request_body;

        auto response = Http1_1ResponseBuilder::ok()
            .header("Server", "Galay-HTTPS-Import/1.0")
            .header("Connection", keep_alive ? "keep-alive" : "close")
            .text(response_body)
            .build();

        while (true) {
            auto send_result = co_await writer.send_response(response);
            if (!send_result) {
                (void)co_await conn.close();
                co_return;
            }
            if (send_result.value()) {
                break;
            }
        }

        if (!keep_alive) {
            break;
        }
    }

    (void)co_await conn.close();
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = galay::http::example::kDefaultHttpsEchoPort;
    std::string cert_path = "test/test.crt";
    std::string key_path = "test/test.key";

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        cert_path = argv[2];
    }
    if (argc > 3) {
        key_path = argv[3];
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        HttpsServer server(HttpsServerBuilder<>()
            .host("0.0.0.0")
            .port(port)
            .cert_path(cert_path)
            .key_path(key_path)
            .io_scheduler_count(2)
            .build_config());
        std::cout << "Import HTTPS server: https://127.0.0.1:" << port << "\n";
        server.start(https_handler);

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        server.stop();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
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
