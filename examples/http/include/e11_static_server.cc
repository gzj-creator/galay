#include "common/example_common.h"
#include "galay-http/server/http_router.h"
#include "galay-http/server/http_server.h"
#include "galay-http/protoc/http_request.h"
#include "galay-http/builder/http_builder.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace galay::http;
using namespace galay::kernel;

Task<void> indexHandler(HttpConn& conn, HttpRequest req) {
    (void)req;
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "Galay-Static-Example/1.0")
        .html(
            "<html><body>"
            "<h1>Galay Static Server</h1>"
            "<p>Static root mounted at <code>/static</code>.</p>"
            "<p>Try: <a href=\"/static/ResumeDownload.html\">ResumeDownload.html</a></p>"
            "<p>Try: <a href=\"/static/EtagCache.html\">EtagCache.html</a></p>"
            "</body></html>")
        .build();

    auto writer = conn.getWriter();
    while (true) {
        auto result = co_await writer.sendResponse(response);
        if (!result || result.value()) break;
    }

    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = example::kDefaultStaticPort;
    std::string static_dir = "./html";

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) static_dir = argv[2];

    HttpRouter router;
    router.addHandler<HttpMethod::GET>("/", indexHandler);
    router.mount("/static", static_dir);

    HttpServer server(HttpServerBuilder()
        .host("0.0.0.0")
        .port(port)
        .ioSchedulerCount(2)
        .build());
    std::cout << "Static server: http://127.0.0.1:" << port << "/\n";
    std::cout << "Static route:  http://127.0.0.1:" << port << "/static/*\n";
    std::cout << "Directory: " << static_dir << "\n";
    server.start(std::move(router));

    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
