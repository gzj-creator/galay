#include <galay/cpp/galay-http/server/http_server.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace galay::async;
using namespace galay::http;
using namespace galay::kernel;

namespace {

struct ProbeState {
    std::atomic<int> calls{0};
    std::atomic<int> close_count{0};
    std::atomic<int> observed_nodelay{-1};
};

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T86] " << message << "\n";
    std::abort();
}

void require(bool condition, const char* message)
{
    if (!condition) {
        fail(message);
    }
}

uint16_t pickFreePort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("socket failed while picking a free port");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    int pton_ok = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (pton_ok != 1) {
        if (::close(fd) != 0) {
            std::cerr << "[T86] close failed after inet_pton failure: " << std::strerror(errno) << "\n";
        }
        fail("inet_pton failed while picking a free port");
    }

    int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        if (::close(fd) != 0) {
            std::cerr << "[T86] close failed after bind failure: " << std::strerror(errno) << "\n";
        }
        fail("bind failed while picking a free port");
    }

    socklen_t len = sizeof(addr);
    rc = ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    if (rc != 0) {
        if (::close(fd) != 0) {
            std::cerr << "[T86] close failed after getsockname failure: " << std::strerror(errno) << "\n";
        }
        fail("getsockname failed while picking a free port");
    }

    const uint16_t port = ntohs(addr.sin_port);
    if (::close(fd) != 0) {
        fail("close failed after picking a free port");
    }
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
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            fail("socket failed while connecting client");
        }

        int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc == 0) {
            return fd;
        }

        const int err = errno;
        if (::close(fd) != 0) {
            std::cerr << "[T86] close failed after connect failure: " << std::strerror(errno) << "\n";
        }
        if (err == ECONNREFUSED || err == ETIMEDOUT || err == EHOSTUNREACH || err == ENETUNREACH) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        std::cerr << "[T86] connect failed, errno=" << err << "\n";
        std::abort();
    }

    fail("connect retry exhausted");
}

void waitForCount(const std::atomic<int>& value, int expected, const char* message)
{
    for (int i = 0; i < 100; ++i) {
        if (value.load() >= expected) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    fail(message);
}

int readTcpNoDelay(int fd)
{
    int value = 0;
    socklen_t value_len = sizeof(value);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &value_len) != 0) {
        std::cerr << "[T86] getsockopt(TCP_NODELAY) failed: " << std::strerror(errno) << "\n";
        std::abort();
    }
    return value;
}

template<typename SocketType>
class NoDelayProbePlugin final : public plugin::AcceptPlugin<SocketType> {
public:
    explicit NoDelayProbePlugin(ProbeState* state, bool continue_after_probe = true)
        : m_state(state)
        , m_continue_after_probe(continue_after_probe) {}

    Task<bool> handle(Runtime&, SocketType& socket, const Host&) override {
        m_state->observed_nodelay.store(readTcpNoDelay(socket.handle().fd));
        m_state->calls.fetch_add(1);
        co_return m_continue_after_probe;
    }

private:
    ProbeState* m_state;
    bool m_continue_after_probe;
};

Task<void> closeConn(HttpConn conn, ProbeState* state)
{
    auto close_result = co_await conn.close();
    if (!close_result) {
        std::cerr << "[T86] server close failed: " << close_result.error().message() << "\n";
        co_return;
    }
    state->close_count.fetch_add(1);
    co_return;
}

int observePlainServerTcpNoDelay(bool tcp_no_delay)
{
    ProbeState state;
    const uint16_t port = pickFreePort();
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .tcpNoDelay(tcp_no_delay)
        .build());

    require(server.addAcceptPlugin(std::make_unique<NoDelayProbePlugin<TcpSocket>>(&state)),
            "nodelay probe plugin should register");

    server.start([&state](HttpConn conn) -> Task<void> {
        co_await closeConn(std::move(conn), &state);
    });
    require(server.isRunning(), "server should start for nodelay probe");

    const int client_fd = connectWithRetry(port);
    waitForCount(state.calls, 1, "nodelay probe plugin did not observe accepted socket");
    waitForCount(state.close_count, 1, "server handler did not close accepted socket");

    if (::close(client_fd) != 0) {
        server.stop();
        fail("client close failed after nodelay probe");
    }
    server.stop();
    return state.observed_nodelay.load();
}

#ifdef GALAY_SSL_FEATURE_ENABLED
int observeHttpsServerTcpNoDelay(bool tcp_no_delay)
{
    ProbeState state;
    const uint16_t port = pickFreePort();
    HttpsServer server(HttpsServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .tcpNoDelay(tcp_no_delay)
        .build());

    require(server.addAcceptPlugin(
                std::make_unique<NoDelayProbePlugin<galay::ssl::SslSocket>>(&state, false)),
            "https nodelay probe plugin should register");

    server.start([](HttpsConn conn) -> Task<void> {
        auto close_result = co_await conn.close();
        if (!close_result) {
            std::cerr << "[T86] https fallback close failed: " << close_result.error().message() << "\n";
        }
        co_return;
    });
    require(server.isRunning(), "https server should start for nodelay probe");

    const int client_fd = connectWithRetry(port);
    waitForCount(state.calls, 1, "https nodelay probe plugin did not observe accepted socket");

    if (::close(client_fd) != 0) {
        server.stop();
        fail("client close failed after https nodelay probe");
    }
    server.stop();
    return state.observed_nodelay.load();
}
#endif

void test_builder_config_surface()
{
    auto default_http_config = HttpServerBuilder().buildConfig();
    require(default_http_config.tcp_no_delay, "HttpServerConfig should enable TCP_NODELAY by default");

    auto disabled_http_config = HttpServerBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_http_config.tcp_no_delay, "HttpServerBuilder should support disabling TCP_NODELAY");

#ifdef GALAY_SSL_FEATURE_ENABLED
    auto default_https_config = HttpsServerBuilder().buildConfig();
    require(default_https_config.tcp_no_delay, "HttpsServerConfig should enable TCP_NODELAY by default");

    auto disabled_https_config = HttpsServerBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_https_config.tcp_no_delay, "HttpsServerBuilder should support disabling TCP_NODELAY");
#endif
}

void test_plain_server_applies_config_to_accepted_socket()
{
    const int default_nodelay = observePlainServerTcpNoDelay(true);
    require(default_nodelay != 0, "default HttpServer accepted socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observePlainServerTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled HttpServer accepted socket should leave TCP_NODELAY off");
}

#ifdef GALAY_SSL_FEATURE_ENABLED
void test_https_server_applies_config_to_accepted_socket()
{
    const int default_nodelay = observeHttpsServerTcpNoDelay(true);
    require(default_nodelay != 0, "default HttpsServer accepted socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeHttpsServerTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled HttpsServer accepted socket should leave TCP_NODELAY off");
}
#endif

} // namespace

int main()
{
    test_builder_config_surface();
    test_plain_server_applies_config_to_accepted_socket();
#ifdef GALAY_SSL_FEATURE_ENABLED
    test_https_server_applies_config_to_accepted_socket();
#endif

    std::cout << "T86-ServerNoDelayConfig PASS\n";
    return 0;
}
