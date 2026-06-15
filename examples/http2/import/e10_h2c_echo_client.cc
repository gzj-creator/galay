#include "galay-kernel/core/runtime.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

import galay.http2;

using namespace galay::http2;
using namespace galay::kernel;

Task<void> runClient(const std::string& host, uint16_t port) {
    auto client = H2cClientBuilder().build();

    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << "\n";
        co_return;
    }

    auto upgrade_result = co_await client.upgrade("/");
    if (!upgrade_result) {
        std::cerr << "Upgrade failed: " << upgrade_result.error().toString() << "\n";
        co_return;
    }

    auto* manager = client.getConn()->streamManager();
    auto stream = manager->allocateStream();

    std::string body = "Hello from import h2c client!";
    std::vector<Http2HeaderField> headers;
    headers.reserve(6);
    headers.emplace_back(":method", "POST");
    headers.emplace_back(":scheme", "http");
    headers.emplace_back(":authority", host + ":" + std::to_string(port));
    headers.emplace_back(":path", "/echo");
    headers.emplace_back("content-type", "text/plain");
    headers.emplace_back("content-length", std::to_string(body.size()));

    stream->sendHeaders(headers, false, true);
    stream->sendData(body, true);

    bool finished = false;
    while (!finished) {
        auto batch_result = co_await stream->getFrames(16);
        if (!batch_result) {
            std::cerr << "Response stream closed unexpectedly\n";
            co_await client.shutdown();
            co_return;
        }
        auto frames = std::move(batch_result.value());
        for (auto& frame : frames) {
            if (!frame) {
                std::cerr << "Response stream closed unexpectedly\n";
                co_await client.shutdown();
                co_return;
            }
            if ((frame->isHeaders() || frame->isData()) && frame->isEndStream()) {
                finished = true;
                break;
            }
        }
    }

    const auto& response = stream->response();
    std::cout << "Status: " << response.status << "\n";
    std::cout << "Body: " << response.body << "\n";
    co_await client.shutdown();
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 9080;
    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = static_cast<uint16_t>(std::atoi(argv[2]));
    }

    try {
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        runtime.start();
        auto join = runtime.spawn(runClient(host, port));
        if (!join) {
            runtime.stop();
            return 1;
        }
        auto result = join->join();
        if (!result) {
            runtime.stop();
            return 1;
        }
        runtime.stop();
    } catch (const std::exception& e) {
        std::cerr << "Client error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
