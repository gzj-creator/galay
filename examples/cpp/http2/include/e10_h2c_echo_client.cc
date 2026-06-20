/**
 * @file E10-H2cEchoClient.cc
 * @brief h2c (HTTP/2 over cleartext) Echo 客户端示例
 * @details 通过 HTTP/1.1 Upgrade 升级后，使用 StreamManager 发送 Echo 请求
 *
 * 测试方法:
 *   # 先启动 Echo Server
 *   ./E9-H2cEchoServer 8080
 *
 *   # 然后运行客户端
 *   ./E10-H2cEchoClient localhost 8080
 */

#include <galay/cpp/galay-http2/client/h2c_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <iostream>
#include <vector>

using namespace galay::http2;
using namespace galay::kernel;

Task<void> runClient(const std::string& host, uint16_t port) {
    auto client = H2cClientBuilder().build();

    std::cout << "Connecting to " << host << ":" << port << "...\n";

    // 连接
    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << "\n";
        co_return;
    }
    std::cout << "Connected!\n";

    // 升级到 HTTP/2（内部启动 StreamManager）
    std::cout << "Upgrading to HTTP/2...\n";
    auto upgrade_result = co_await client.upgrade("/");
    if (!upgrade_result) {
        std::cerr << "Upgrade failed: " << upgrade_result.error().toString() << "\n";
        co_return;
    }
    std::cout << "Upgraded to HTTP/2!\n\n";

    auto* mgr = client.getConn()->streamManager();

    // 发送 POST /echo
    std::string body = "Hello from H2cEchoClient!";
    const std::string authority = host + ":" + std::to_string(port);
    const std::string content_length = std::to_string(body.size());
    std::vector<Http2HeaderField> headers;
    headers.reserve(6);
    headers.emplace_back(":method", "POST");
    headers.emplace_back(":scheme", "http");
    headers.emplace_back(":authority", authority);
    headers.emplace_back(":path", "/echo");
    headers.emplace_back("content-type", "text/plain");
    headers.emplace_back("content-length", content_length);

    auto stream = mgr->allocateStream();

    std::cout << "=== POST /echo ===\n";
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
    auto& response = stream->response();

    std::cout << "Status: " << response.status << "\n";
    std::cout << "Body: " << response.body << "\n\n";

    // 优雅关闭
    co_await client.shutdown();
    std::cout << "Connection closed.\n";

    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 8080;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);

    std::cout << "========================================\n";
    std::cout << "H2c (HTTP/2 Cleartext) Echo Client Example\n";
    std::cout << "========================================\n";

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
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
