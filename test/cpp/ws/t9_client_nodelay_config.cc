#include <galay/cpp/galay-ws/client/ws_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using namespace galay::kernel;
using namespace galay::websocket;

namespace {

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T88] " << message << "\n";
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
            std::cerr << "[T88] close listener failed: " << std::strerror(errno) << "\n";
        }
    }

    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    uint16_t port() const { return m_port; }

private:
    int m_fd = -1;
    uint16_t m_port = 0;
};

int readTcpNoDelay(int fd)
{
    int value = 0;
    socklen_t value_len = sizeof(value);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &value_len) != 0) {
        std::cerr << "[T88] getsockopt(TCP_NODELAY) failed: " << std::strerror(errno) << "\n";
        std::abort();
    }
    return value;
}

int observeWsClientTcpNoDelay(bool tcp_no_delay)
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    LoopbackListener listener;
    WsClient client = WsClientBuilder().tcpNoDelay(tcp_no_delay).build();

    auto connect_result = runtime.blockOn(
        client.connect("ws://127.0.0.1:" + std::to_string(listener.port()) + "/ws"));
    require(connect_result.has_value(), "runtime should run WsClient connect task");
    require(connect_result.value().has_value(), "WsClient connect should succeed against loopback listener");

    auto* socket = client.getSocket();
    require(socket != nullptr, "WsClient should expose connected socket");
    const int observed = readTcpNoDelay(socket->handle().fd);

    auto close_result = runtime.blockOn(client.close());
    require(close_result.has_value(), "runtime should run WsClient close task");
    require(close_result.value().has_value(), "WsClient close should succeed");
    runtime.stop();
    return observed;
}

#ifdef GALAY_SSL_FEATURE_ENABLED
int observeWssClientTcpNoDelay(bool tcp_no_delay)
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    LoopbackListener listener;
    WssClient client = WssClientBuilder().tcpNoDelay(tcp_no_delay).build();

    auto connect_result = runtime.blockOn(
        client.connect("wss://127.0.0.1:" + std::to_string(listener.port()) + "/ws"));
    require(connect_result.has_value(), "runtime should run WssClient connect task");
    require(connect_result.value().has_value(), "WssClient connect should succeed against loopback listener");

    auto* socket = client.getSocket();
    require(socket != nullptr, "WssClient should expose connected socket");
    const int observed = readTcpNoDelay(socket->handle().fd);

    auto close_result = runtime.blockOn(client.close());
    require(close_result.has_value(), "runtime should run WssClient close task");
    require(close_result.value().has_value(), "WssClient close should succeed");
    runtime.stop();
    return observed;
}
#endif

void test_builder_config_surface()
{
    auto default_ws_config = WsClientBuilder().buildConfig();
    require(default_ws_config.tcp_no_delay, "WsClientConfig should enable TCP_NODELAY by default");

    auto disabled_ws_config = WsClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_ws_config.tcp_no_delay, "WsClientBuilder should support disabling TCP_NODELAY");

#ifdef GALAY_SSL_FEATURE_ENABLED
    auto default_wss_config = WssClientBuilder().buildConfig();
    require(default_wss_config.tcp_no_delay, "WssClientConfig should enable TCP_NODELAY by default");

    auto disabled_wss_config = WssClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_wss_config.tcp_no_delay, "WssClientBuilder should support disabling TCP_NODELAY");
#endif
}

void test_plain_client_applies_config_to_connected_socket()
{
    const int default_nodelay = observeWsClientTcpNoDelay(true);
    require(default_nodelay != 0, "default WsClient socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeWsClientTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled WsClient socket should leave TCP_NODELAY off");
}

#ifdef GALAY_SSL_FEATURE_ENABLED
void test_wss_client_applies_config_to_connected_socket()
{
    const int default_nodelay = observeWssClientTcpNoDelay(true);
    require(default_nodelay != 0, "default WssClient socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeWssClientTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled WssClient socket should leave TCP_NODELAY off");
}
#endif

} // namespace

int main()
{
    test_builder_config_surface();
    test_plain_client_applies_config_to_connected_socket();
#ifdef GALAY_SSL_FEATURE_ENABLED
    test_wss_client_applies_config_to_connected_socket();
#endif

    std::cout << "T88-WsClientNoDelayConfig PASS\n";
    return 0;
}
