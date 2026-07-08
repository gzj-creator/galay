/**
 * @file E14-H2EchoClient.cpp
 * @brief h2 (HTTP/2 over TLS) Echo 客户端示例
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <iostream>
#include <cstdlib>
#include <thread>
#include <chrono>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include <galay/cpp/galay-http2/client/h2_client.h>
#endif

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

    std::string alpn = client.getALPNProtocol();
    std::cout << "ALPN: " << (alpn.empty() ? "(empty)" : alpn) << "\n";
    if (alpn != "h2") {
        std::cerr << "ALPN is not h2\n";
        co_await client.close();
        co_return;
    }

    std::string body = "Hello from H2EchoClient!";
    auto stream = client.post("/echo", body, "text/plain");
    if (!stream) {
        std::cerr << "Request failed: stream create error\n";
        co_await client.close();
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
        co_await client.close();
        co_return;
    }

    auto& response = stream->response();
    std::cout << "Status: " << response.status << "\n";
    std::cout << "Body: " << response.body << "\n";

    co_await client.close();
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 9443;
    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));

    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Echo Client Example\n";
    std::cout << "========================================\n";
    std::cout << "Target: " << host << ":" << port << "\n";
    std::cout << "========================================\n";

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "No IO scheduler available\n";
        return 1;
    }

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
    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_BUILD_SSL=ON\n";
    return 0;
}

#endif
