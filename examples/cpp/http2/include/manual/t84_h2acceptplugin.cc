#include <galay/cpp/galay-http/plugin/common/defn.h>
#include <galay/cpp/galay-http2/server/http2_server.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <unistd.h>

using namespace galay::async;
using namespace galay::http::plugin;
using namespace galay::http2;
using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T84] " << message << "\n";
    std::abort();
}

std::string resolveHttp2Asset(const char* name)
{
    const char* dirs[] = {
        "test/cpp/http2",
        "../test/cpp/http2",
        "../../test/cpp/http2",
        "../source/test/cpp/http2",
        "../../source/test/cpp/http2",
    };

    for (const char* dir : dirs) {
        std::filesystem::path path = std::filesystem::path(dir) / name;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return path.string();
        }
    }

    fail("missing http2 TLS test asset");
}

uint16_t pickFreePort()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("socket failed while picking a free port");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    int pton_ok = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (pton_ok != 1) {
        ::close(fd);
        fail("inet_pton failed while picking a free port");
    }

    int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        ::close(fd);
        fail("bind failed while picking a free port");
    }

    socklen_t len = sizeof(addr);
    rc = ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    if (rc != 0) {
        ::close(fd);
        fail("getsockname failed while picking a free port");
    }

    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

int connectWithRetry(uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    int pton_ok = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (pton_ok != 1) {
        fail("inet_pton failed while connecting client");
    }

    for (int attempt = 0; attempt < 100; ++attempt) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            fail("socket failed while connecting client");
        }

        int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc == 0) {
            return fd;
        }

        int err = errno;
        ::close(fd);
        if (err == ECONNREFUSED || err == ETIMEDOUT || err == EHOSTUNREACH ||
            err == ENETUNREACH) {
            std::this_thread::sleep_for(20ms);
            continue;
        }

        std::cerr << "[T84] connect failed, errno=" << err
                  << " (" << std::strerror(err) << ")\n";
        std::abort();
    }

    fail("connect retry exhausted");
}

void openAndClose(uint16_t port)
{
    int fd = connectWithRetry(port);
    ::close(fd);
}

void waitForCount(const std::atomic<int>& value, int expected, const char* message)
{
    for (int i = 0; i < 100; ++i) {
        if (value.load() >= expected) {
            return;
        }
        std::this_thread::sleep_for(20ms);
    }
    fail(message);
}

void waitForStableCount(const std::atomic<int>& value,
                        int expected,
                        std::chrono::milliseconds duration,
                        const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load() != expected) {
            fail(message);
        }
        std::this_thread::sleep_for(10ms);
    }
}

Task<void> unusedStreamHandler(Http2Stream::ptr)
{
    co_return;
}

template<typename SocketType>
class OneAndBlockPlugin final : public AcceptPlugin<SocketType> {
public:
    OneAndBlockPlugin(std::atomic<int>* calls,
                      std::atomic<int>* start_count,
                      std::atomic<int>* stop_count)
        : m_calls(calls)
        , m_start_count(start_count)
        , m_stop_count(stop_count) {}

    bool start(Runtime&) override {
        m_start_count->fetch_add(1);
        return true;
    }

    void stop() noexcept override {
        m_stop_count->fetch_add(1);
    }

    Task<bool> handle(Runtime&, SocketType&, const Host&) override {
        int attempt = m_calls->fetch_add(1) + 1;
        co_return attempt == 1;
    }

private:
    std::atomic<int>* m_calls;
    std::atomic<int>* m_start_count;
    std::atomic<int>* m_stop_count;
};

template<typename SocketType>
class CountingPlugin final : public AcceptPlugin<SocketType> {
public:
    CountingPlugin(std::atomic<int>* calls,
                   std::atomic<int>* start_count,
                   std::atomic<int>* stop_count,
                   bool result)
        : m_calls(calls)
        , m_start_count(start_count)
        , m_stop_count(stop_count)
        , m_result(result) {}

    bool start(Runtime&) override {
        m_start_count->fetch_add(1);
        return true;
    }

    void stop() noexcept override {
        m_stop_count->fetch_add(1);
    }

    Task<bool> handle(Runtime&, SocketType&, const Host&) override {
        m_calls->fetch_add(1);
        co_return m_result;
    }

private:
    std::atomic<int>* m_calls;
    std::atomic<int>* m_start_count;
    std::atomic<int>* m_stop_count;
    bool m_result;
};

void test_h2c_accept_plugin_blocks_before_downstream_plugin()
{
    std::atomic<int> gate_count{0};
    std::atomic<int> downstream_count{0};
    std::atomic<int> gate_start_count{0};
    std::atomic<int> gate_stop_count{0};
    std::atomic<int> downstream_start_count{0};
    std::atomic<int> downstream_stop_count{0};
    uint16_t port = pickFreePort();

    H2cServer server(H2cServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(2)
        .computeSchedulerCount(0)
        .streamHandler(unusedStreamHandler)
        .build());

    bool registered_blacklist =
        server.addAcceptPlugin(std::make_unique<OneAndBlockPlugin<TcpSocket>>(
            &gate_count, &gate_start_count, &gate_stop_count));
    bool registered_downstream = server.addAcceptPlugin(
        std::make_unique<CountingPlugin<TcpSocket>>(
            &downstream_count, &downstream_start_count, &downstream_stop_count, true));
    if (!registered_blacklist || !registered_downstream) {
        fail("h2c accept plugins should register before start");
    }

    server.start();

    openAndClose(port);
    waitForCount(downstream_count, 1, "first h2c connection should reach downstream plugin");

    openAndClose(port);
    waitForStableCount(downstream_count, 1, 150ms,
                       "blacklisted h2c connection should stop downstream plugin");

    bool registered_after_start = server.addAcceptPlugin(
        std::make_unique<CountingPlugin<TcpSocket>>(
            &downstream_count, &downstream_start_count, &downstream_stop_count, true));
    if (registered_after_start) {
        fail("h2c accept plugin registration after start should be rejected");
    }

    server.stop();
    waitForCount(gate_start_count, 1, "h2c gate plugin start should run once");
    waitForCount(downstream_start_count, 1, "h2c downstream plugin start should run once");
    waitForCount(gate_stop_count, 1, "h2c gate plugin stop should run once");
    waitForCount(downstream_stop_count, 1, "h2c downstream plugin stop should run once");
}

#ifdef GALAY_SSL_FEATURE_ENABLED
void test_h2_accept_plugin_blocks_before_downstream_plugin()
{
    std::atomic<int> gate_count{0};
    std::atomic<int> downstream_count{0};
    std::atomic<int> gate_start_count{0};
    std::atomic<int> gate_stop_count{0};
    std::atomic<int> downstream_start_count{0};
    std::atomic<int> downstream_stop_count{0};
    uint16_t port = pickFreePort();
    const std::string cert_path = resolveHttp2Asset("test.crt");
    const std::string key_path = resolveHttp2Asset("test.key");

    H2Server server(H2ServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .certPath(cert_path)
        .keyPath(key_path)
        .ioSchedulerCount(2)
        .computeSchedulerCount(0)
        .streamHandler(unusedStreamHandler)
        .build());

    bool registered_blacklist = server.addAcceptPlugin(
        std::make_unique<OneAndBlockPlugin<galay::ssl::SslSocket>>(
            &gate_count, &gate_start_count, &gate_stop_count));
    bool registered_downstream = server.addAcceptPlugin(
        std::make_unique<CountingPlugin<galay::ssl::SslSocket>>(
            &downstream_count, &downstream_start_count, &downstream_stop_count, true));
    if (!registered_blacklist || !registered_downstream) {
        fail("h2 accept plugins should register before start");
    }

    server.start();

    openAndClose(port);
    waitForCount(downstream_count, 1, "first h2 connection should reach downstream plugin");

    openAndClose(port);
    waitForStableCount(downstream_count, 1, 150ms,
                       "blacklisted h2 connection should stop downstream plugin");

    bool registered_after_start = server.addAcceptPlugin(
        std::make_unique<CountingPlugin<galay::ssl::SslSocket>>(
            &downstream_count, &downstream_start_count, &downstream_stop_count, true));
    if (registered_after_start) {
        fail("h2 accept plugin registration after start should be rejected");
    }

    server.stop();
    waitForCount(gate_start_count, 1, "h2 gate plugin start should run once");
    waitForCount(downstream_start_count, 1, "h2 downstream plugin start should run once");
    waitForCount(gate_stop_count, 1, "h2 gate plugin stop should run once");
    waitForCount(downstream_stop_count, 1, "h2 downstream plugin stop should run once");
}
#endif

} // namespace

int main()
{
    test_h2c_accept_plugin_blocks_before_downstream_plugin();
#ifdef GALAY_SSL_FEATURE_ENABLED
    test_h2_accept_plugin_blocks_before_downstream_plugin();
#endif
    std::cout << "T84-H2AcceptPlugin PASS\n";
    return 0;
}
