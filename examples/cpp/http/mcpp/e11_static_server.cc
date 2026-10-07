#include "common/example_common.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

import galay.http;

using namespace galay::http;
using namespace galay::kernel;

Task<void> index_handler(HttpConn& conn, HttpRequest req) {
    (void)req;
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "Galay-Static-Import/1.0")
        .html(
            "<html><body>"
            "<h1>Import Static Server</h1>"
            "<p>Static root mounted at <code>/static</code>.</p>"
            "</body></html>")
        .build();

    auto writer = conn.get_writer();
    (void)co_await writer.send_response(response);
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = galay::http::example::kDefaultStaticPort;
    std::string static_dir = "./html";

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        static_dir = argv[2];
    }

    HttpRouter router;
    router.add_handler<HttpMethod::GET>("/", index_handler);
    router.mount("/static", static_dir);

    HttpServer server(HttpServerBuilder()
        .host("0.0.0.0")
        .port(port)
        .io_scheduler_count(2)
        .build());
    std::cout << "Import static server: http://127.0.0.1:" << port << "/\n";
    server.start(std::move(router));

    while (server.is_running()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
