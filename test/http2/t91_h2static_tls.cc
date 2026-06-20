/**
 * @file t91_h2static_tls.cc
 * @brief HTTP/2 over TLS static file GET regression test
 */

#include <galay/cpp/galay-http2/client/h2_client.h>
#include <galay/cpp/galay-http2/server/http2_server.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace galay::http2;
using namespace galay::kernel;

#ifdef GALAY_SSL_FEATURE_ENABLED

namespace {

std::atomic<bool> g_done{false};
std::atomic<bool> g_ok{false};

std::string responseHeader(const Http2Stream::ptr& stream, const std::string& name)
{
    if (!stream) {
        return "";
    }
    for (const auto& header : stream->response().headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return "";
}

uint16_t reserveFreePort()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return 0;
    }

    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

Task<void> runClient(uint16_t port)
{
    H2Client client(H2ClientBuilder().verifyPeer(false).build());
    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result || !connect_result.value()) {
        std::cerr << "[T91] connect failed has_value="
                  << connect_result.has_value()
                  << " alpn=" << client.getALPNProtocol() << "\n";
        g_done = true;
        co_return;
    }

    auto stream = client.get("/files/hello.txt");
    if (!stream) {
        std::cerr << "[T91] GET stream allocation failed\n";
        g_done = true;
        co_return;
    }

    std::cout << "[T91] issued GET\n";
    auto complete = co_await stream->waitResponseComplete();
    if (!complete) {
        std::cerr << "[T91] GET waitResponseComplete failed\n";
        g_done = true;
        co_return;
    }

    if (stream->response().status != 200 ||
        stream->response().body != "hello tls" ||
        responseHeader(stream, "content-type") != "text/plain" ||
        responseHeader(stream, "content-length") != "9") {
        std::cerr << "[T91] GET /files/hello.txt failed status="
                  << stream->response().status
                  << " body_size=" << stream->response().body.size()
                  << " content-length=" << responseHeader(stream, "content-length")
                  << "\n";
        g_done = true;
        co_return;
    }

    std::cout << "[T91] GET completed\n";
    co_await client.close();
    g_ok = true;
    g_done = true;
    co_return;
}

} // namespace

int main()
{
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() /
        ("galay-h2-static-tls-" + std::to_string(::getpid()));
    const auto root = base / "public";
    fs::create_directories(root);
    {
        std::ofstream(root / "hello.txt") << "hello tls";
    }

    const uint16_t port = reserveFreePort();
    if (port == 0) {
        std::cerr << "[T91] failed to reserve free port\n";
        fs::remove_all(base);
        return 1;
    }

    H2Server server(H2ServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .certPath("test/http2/test.crt")
        .keyPath("test/http2/test.key")
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .staticFiles("/files", H2StaticFileConfig{.root = root})
        .activeConnHandler([](Http2ConnContext& ctx) -> Task<void> {
            while (true) {
                auto streams = co_await ctx.getActiveStreams(16);
                if (!streams) {
                    break;
                }
                for (auto& stream : *streams) {
                    auto events = stream->takeEvents();
                    if (!hasHttp2StreamEvent(events, Http2StreamEvent::RequestComplete)) {
                        continue;
                    }
                    stream->sendHeaders(
                        Http2Headers().status(500).contentType("text/plain").contentLength(0),
                        true,
                        true);
                }
            }
            co_return;
        })
        .build());

    server.start();
    if (!server.isRunning()) {
        std::cerr << "[T91] server failed to start\n";
        fs::remove_all(base);
        return 1;
    }
    std::cout << "[T91] server started\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();
    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "[T91] missing IO scheduler\n";
        server.stop();
        return 1;
    }

    scheduleTask(scheduler, runClient(port));

    for (int i = 0; i < 200; ++i) {
        if (g_done.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    runtime.stop();
    server.stop();
    fs::remove_all(base);

    if (!g_done.load(std::memory_order_acquire)) {
        std::cerr << "[T91] client timed out\n";
        return 1;
    }
    if (!g_ok.load(std::memory_order_acquire)) {
        return 1;
    }

    std::cout << "t91_h2static_tls PASS\n";
    return 0;
}

#else

int main()
{
    std::cout << "T91-H2StaticTls SKIP (SSL disabled)\n";
    return 0;
}

#endif
