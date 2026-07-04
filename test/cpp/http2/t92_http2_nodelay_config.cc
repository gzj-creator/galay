#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#define private public
#include <galay/cpp/galay-http2/client/h2c_client.h>
#include <galay/cpp/galay-http2/client/h2_client.h>
#include <galay/cpp/galay-http2/server/http2_server.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#undef private

using namespace galay::async;
using namespace galay::http2;
using namespace galay::kernel;

namespace {

struct ProbeState {
    std::atomic<int> calls{0};
    std::atomic<int> observed_nodelay{-1};
};

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T92] " << message << "\n";
    std::abort();
}

void require(bool condition, const char* message)
{
    if (!condition) {
        fail(message);
    }
}

class LoopbackListener {
public:
    LoopbackListener()
    {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) {
            fail("socket failed while creating listener");
        }

        int reuse = 1;
        if (::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            fail("setsockopt(SO_REUSEADDR) failed while creating listener");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        int pton_ok = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (pton_ok != 1) {
            fail("inet_pton failed while creating listener");
        }

        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            fail("bind failed while creating listener");
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            fail("getsockname failed while creating listener");
        }
        m_port = ntohs(addr.sin_port);

        if (::listen(m_fd, 16) != 0) {
            fail("listen failed while creating listener");
        }
    }

    ~LoopbackListener()
    {
        if (m_fd >= 0 && ::close(m_fd) != 0) {
            std::cerr << "[T92] close listener failed: " << std::strerror(errno) << "\n";
        }
    }

    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    uint16_t port() const { return m_port; }

private:
    int m_fd = -1;
    uint16_t m_port = 0;
};

uint16_t pickFreePort()
{
    LoopbackListener listener;
    return listener.port();
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
            std::cerr << "[T92] close failed after connect failure: " << std::strerror(errno) << "\n";
        }
        if (err == ECONNREFUSED || err == ETIMEDOUT || err == EHOSTUNREACH || err == ENETUNREACH) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        std::cerr << "[T92] connect failed, errno=" << err << "\n";
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
        std::cerr << "[T92] getsockopt(TCP_NODELAY) failed: " << std::strerror(errno) << "\n";
        std::abort();
    }
    return value;
}

template<typename SocketType>
class NoDelayProbePlugin final : public galay::http::plugin::AcceptPlugin<SocketType> {
public:
    explicit NoDelayProbePlugin(ProbeState* state)
        : m_state(state) {}

    Task<bool> handle(Runtime&, SocketType& socket, const Host&) override {
        m_state->observed_nodelay.store(readTcpNoDelay(socket.handle().fd));
        m_state->calls.fetch_add(1);
        co_return false;
    }

private:
    ProbeState* m_state;
};

Task<std::expected<void, IOError>> closeTcpSocket(TcpSocket* socket)
{
    if (socket == nullptr) {
        co_return std::unexpected(IOError(kNotReady, 0));
    }
    co_return co_await socket->close();
}

int observeH2cClientTcpNoDelay(bool tcp_no_delay)
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    LoopbackListener listener;
    H2cClient client(H2cClientBuilder().tcpNoDelay(tcp_no_delay).build());

    auto connect_result = runtime.blockOn(client.connect("127.0.0.1", listener.port()));
    require(connect_result.has_value(), "runtime should run H2cClient connect task");
    require(connect_result.value().has_value(), "H2cClient connect should succeed against loopback listener");
    require(client.m_socket != nullptr, "H2cClient should keep socket before upgrade");
    const int observed = readTcpNoDelay(client.m_socket->handle().fd);

    auto close_result = runtime.blockOn(closeTcpSocket(client.m_socket.get()));
    require(close_result.has_value(), "runtime should run H2cClient socket close task");
    require(close_result.value().has_value(), "H2cClient socket close should succeed");
    runtime.stop();
    return observed;
}

int observeH2cServerTcpNoDelay(bool tcp_no_delay)
{
    ProbeState state;
    const uint16_t port = pickFreePort();
    H2cServer server(H2cServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .tcpNoDelay(tcp_no_delay)
        .streamHandler([](Http2Stream::ptr) -> Task<void> {
            co_return;
        })
        .build());

    require(server.addAcceptPlugin(std::make_unique<NoDelayProbePlugin<TcpSocket>>(&state)),
            "h2c nodelay probe plugin should register");

    server.start();
    require(server.isRunning(), "h2c server should start for nodelay probe");

    const int client_fd = connectWithRetry(port);
    waitForCount(state.calls, 1, "h2c nodelay probe plugin did not observe accepted socket");

    if (::close(client_fd) != 0) {
        server.stop();
        fail("client close failed after h2c nodelay probe");
    }
    server.stop();
    return state.observed_nodelay.load();
}

#ifdef GALAY_SSL_FEATURE_ENABLED
int observeH2ServerTcpNoDelay(bool tcp_no_delay)
{
    ProbeState state;
    const uint16_t port = pickFreePort();
    H2Server server(H2ServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .certPath("test/cpp/http2/test.crt")
        .keyPath("test/cpp/http2/test.key")
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .tcpNoDelay(tcp_no_delay)
        .streamHandler([](Http2Stream::ptr) -> Task<void> {
            co_return;
        })
        .build());

    require(server.addAcceptPlugin(
                std::make_unique<NoDelayProbePlugin<galay::ssl::SslSocket>>(&state)),
            "h2 nodelay probe plugin should register");

    server.start();
    require(server.isRunning(), "h2 server should start for nodelay probe");

    const int client_fd = connectWithRetry(port);
    waitForCount(state.calls, 1, "h2 nodelay probe plugin did not observe accepted socket");

    if (::close(client_fd) != 0) {
        server.stop();
        fail("client close failed after h2 nodelay probe");
    }
    server.stop();
    return state.observed_nodelay.load();
}

Task<void> idleH2ConnectionHandler(Http2ConnContext& ctx)
{
    while (true) {
        auto streams = co_await ctx.getActiveStreams(1);
        if (!streams) {
            break;
        }
    }
    co_return;
}

int observeH2ClientTcpNoDelay(bool tcp_no_delay)
{
    const uint16_t port = pickFreePort();
    H2Server server(H2ServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .certPath("test/cpp/http2/test.crt")
        .keyPath("test/cpp/http2/test.key")
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .activeConnHandler(idleH2ConnectionHandler)
        .build());

    server.start();
    require(server.isRunning(), "h2 server should start for client nodelay probe");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    H2Client client(H2ClientBuilder()
        .verifyPeer(false)
        .tcpNoDelay(tcp_no_delay)
        .build());

    auto connect_result = runtime.blockOn(client.connect("127.0.0.1", port));
    require(connect_result.has_value(), "runtime should run H2Client connect task");
    require(connect_result.value().has_value(), "H2Client connect should succeed against local h2 server");
    require(client.m_conn != nullptr, "H2Client should finalize connected transport");
    const int observed = readTcpNoDelay(client.m_conn->socket().handle().fd);

    auto close_result = runtime.blockOn(client.close());
    require(close_result.has_value(), "runtime should run H2Client close task");
    require(close_result.value().has_value(), "H2Client close should succeed");
    runtime.stop();
    server.stop();
    return observed;
}
#endif

void test_builder_config_surface()
{
    auto default_h2c_client = H2cClientBuilder().buildConfig();
    require(default_h2c_client.tcp_no_delay, "H2cClientConfig should enable TCP_NODELAY by default");

    auto disabled_h2c_client = H2cClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_h2c_client.tcp_no_delay, "H2cClientBuilder should support disabling TCP_NODELAY");

    auto default_h2c_server = H2cServerBuilder().buildConfig();
    require(default_h2c_server.tcp_no_delay, "H2cServerConfig should enable TCP_NODELAY by default");

    auto disabled_h2c_server = H2cServerBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_h2c_server.tcp_no_delay, "H2cServerBuilder should support disabling TCP_NODELAY");

#ifdef GALAY_SSL_FEATURE_ENABLED
    auto default_h2_client = H2ClientBuilder().buildConfig();
    require(default_h2_client.tcp_no_delay, "H2ClientConfig should enable TCP_NODELAY by default");

    auto disabled_h2_client = H2ClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_h2_client.tcp_no_delay, "H2ClientBuilder should support disabling TCP_NODELAY");

    auto default_h2_server = H2ServerBuilder().buildConfig();
    require(default_h2_server.tcp_no_delay, "H2ServerConfig should enable TCP_NODELAY by default");

    auto disabled_h2_server = H2ServerBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_h2_server.tcp_no_delay, "H2ServerBuilder should support disabling TCP_NODELAY");
#endif
}

void test_h2c_client_applies_config_to_connected_socket()
{
    const int default_nodelay = observeH2cClientTcpNoDelay(true);
    require(default_nodelay != 0, "default H2cClient socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeH2cClientTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled H2cClient socket should leave TCP_NODELAY off");
}

void test_h2c_server_applies_config_to_accepted_socket()
{
    const int default_nodelay = observeH2cServerTcpNoDelay(true);
    require(default_nodelay != 0, "default H2cServer accepted socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeH2cServerTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled H2cServer accepted socket should leave TCP_NODELAY off");
}

#ifdef GALAY_SSL_FEATURE_ENABLED
void test_h2_server_applies_config_to_accepted_socket()
{
    const int default_nodelay = observeH2ServerTcpNoDelay(true);
    require(default_nodelay != 0, "default H2Server accepted socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeH2ServerTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled H2Server accepted socket should leave TCP_NODELAY off");
}

void test_h2_client_applies_config_to_connected_socket()
{
    const int default_nodelay = observeH2ClientTcpNoDelay(true);
    require(default_nodelay != 0, "default H2Client socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeH2ClientTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled H2Client socket should leave TCP_NODELAY off");
}
#endif

} // namespace

int main()
{
    test_builder_config_surface();
    test_h2c_client_applies_config_to_connected_socket();
    test_h2c_server_applies_config_to_accepted_socket();
#ifdef GALAY_SSL_FEATURE_ENABLED
    test_h2_server_applies_config_to_accepted_socket();
    test_h2_client_applies_config_to_connected_socket();
#endif

    std::cout << "T92-Http2NoDelayConfig PASS\n";
    return 0;
}
