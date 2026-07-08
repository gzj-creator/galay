#include "common/example_common.h"
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <cstdlib>
#include <iostream>
#include <string>

import galay.http2;

#ifdef GALAY_SSL_FEATURE_ENABLED

using namespace galay::http2;
using namespace galay::kernel;

Task<void> runClient(const std::string& host, uint16_t port) {
    H2Client<> client(H2ClientBuilder()
        .verifyPeer(false)
        .build());

    auto connect_task_result = co_await client.connect(host, port);
    if (!connect_task_result) {
        std::cerr << "Connect failed: " << connect_task_result.error().message() << "\n";
        co_return;
    }

    auto connect_result = std::move(connect_task_result.value());
    if (!connect_result || !connect_result.value()) {
        if (!connect_result) {
            std::cerr << "Connect failed: " << static_cast<int>(connect_result.error()) << "\n";
        } else {
            std::cerr << "Connect failed: false\n";
        }
        co_return;
    }

    const std::string alpn = client.getALPNProtocol();
    std::cout << "ALPN: " << (alpn.empty() ? "(empty)" : alpn) << "\n";
    if (alpn != "h2") {
        std::cerr << "ALPN is not h2\n";
        (void)co_await client.close();
        co_return;
    }

    auto stream = client.post("/echo", "Hello from import H2 client!", "text/plain");
    if (!stream) {
        std::cerr << "Request failed: stream create error\n";
        (void)co_await client.close();
        co_return;
    }

    bool finished = false;
    while (!finished) {
        auto batch_result = co_await stream->getFrames(16);
        if (!batch_result) {
            break;
        }
        auto frames = std::move(batch_result.value());
        bool stream_closed = false;
        for (auto& frame : frames) {
            if (!frame) {
                stream_closed = true;
                break;
            }
            if ((frame->isHeaders() || frame->isData()) && frame->isEndStream()) {
                finished = true;
                break;
            }
        }
        if (stream_closed) {
            break;
        }
    }

    if (!finished) {
        std::cerr << "No response\n";
        (void)co_await client.close();
        co_return;
    }

    const auto& response = stream->response();
    std::cout << "Status: " << response.status << "\n";
    std::cout << "Body: " << response.body << "\n";
    (void)co_await client.close();
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = galay::http::example::kDefaultH2EchoPort;

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

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
