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

void signalHandler(int) {
    g_running = false;
}

Task<void> httpsHandler(HttpConnImpl<galay::ssl::SslSocket> conn) {
    auto reader = conn.getReader();
    auto writer = conn.getWriter();

    while (true) {
        HttpRequest request;
        while (true) {
            auto read_result = co_await reader.getRequest(request);
            if (!read_result) {
                (void)co_await conn.close();
                co_return;
            }
            if (read_result.value()) {
                break;
            }
        }

        const bool keep_alive =
            request.header().isKeepAlive() && !request.header().isConnectionClose();
        const std::string request_body = request.getBodyStr();
        const std::string response_body = request_body.empty()
            ? "Echo: (empty body)"
            : "Echo: " + request_body;

        auto response = Http1_1ResponseBuilder::ok()
            .header("Server", "Galay-HTTPS-Import/1.0")
            .header("Connection", keep_alive ? "keep-alive" : "close")
            .text(response_body)
            .build();

        while (true) {
            auto send_result = co_await writer.sendResponse(response);
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

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpsServer server(HttpsServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .certPath(cert_path)
            .keyPath(key_path)
            .ioSchedulerCount(2)
            .build());
        std::cout << "Import HTTPS server: https://127.0.0.1:" << port << "\n";
        server.start(httpsHandler);

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
