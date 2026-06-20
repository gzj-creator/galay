#include "common/example_common.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

import galay.http;

using namespace galay::http;
using namespace galay::http::plugin;
using namespace galay::kernel;
using namespace std::chrono_literals;

Task<void> indexHandler(HttpConn& conn, HttpRequest req) {
    (void)req;
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "Galay-Blacklist-Import/1.0")
        .text("blacklist import demo ok\n")
        .build();

    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);
    if (!result) {
        std::cerr << "Failed to send response: " << result.error().message() << "\n";
    }
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = galay::http::example::kDefaultBlacklistPort;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    BlackListConfig config;
    BlackListConfig::IntervalBlockPolicy policy;
    policy.max_attempts_per_interval = 2;
    policy.interval = 10s;
    policy.block_duration = 5s;
    policy.reset_counter_after_unblock = true;
    config.policy = policy;

    HttpRouter router;
    router.addHandler<HttpMethod::GET>("/", indexHandler);

    HttpServer server(HttpServerBuilder()
        .host("0.0.0.0")
        .port(port)
        .ioSchedulerCount(2)
        .computeSchedulerCount(1)
        .build());

    if (!server.addAcceptPlugin(
            std::make_unique<BlackList<galay::async::TcpSocket>>(config))) {
        std::cerr << "Failed to register blacklist accept plugin\n";
        return 1;
    }

    std::cout << "Import blacklist demo server: http://127.0.0.1:" << port << "/\n";
    std::cout << "Policy: allow 2 connections per 10s, then block for 5s\n";

    server.start(std::move(router));

    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
