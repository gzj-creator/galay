#include "common/example_common.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

import galay.http;

using namespace galay::http;
using namespace galay::kernel;

Task<void> echo_handler(HttpConn& conn, HttpRequest req) {
    const std::string request_body = req.get_body_str();
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "Galay-HTTP-Echo-Import/1.0")
        .text(request_body.empty() ? "Echo: (empty body)" : "Echo: " + request_body)
        .build();

    auto writer = conn.get_writer();
    auto result = co_await writer.send_response(response);
    if (!result) {
        std::cerr << "Failed to send response: " << result.error().message() << "\n";
    }
    co_return;
}

Task<void> index_handler(HttpConn& conn, HttpRequest req) {
    (void)req;
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "Galay-HTTP-Echo-Import/1.0")
        .html(
            "<html><body>"
            "<h1>Import Echo Server</h1>"
            "<p>POST to <code>/echo</code> to receive an echo response.</p>"
            "</body></html>")
        .build();

    auto writer = conn.get_writer();
    auto result = co_await writer.send_response(response);
    if (!result) {
        std::cerr << "Failed to send response: " << result.error().message() << "\n";
    }
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = galay::http::example::kDefaultHttpEchoPort;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    try {
        HttpRouter router;
        router.add_handler<HttpMethod::GET>("/", index_handler);
        router.add_handler<HttpMethod::POST>("/echo", echo_handler);

        HttpServer server(HttpServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .backlog(128)
            .build());

        std::cout << "Import echo server: http://127.0.0.1:" << port << "\n";
        server.start(std::move(router));

        while (server.is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
