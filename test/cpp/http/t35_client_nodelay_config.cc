#include <galay/cpp/galay-http/client/http_client.h>
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

using namespace galay::http;
using namespace galay::kernel;

namespace {

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T87] " << message << "\n";
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
            std::cerr << "[T87] close listener failed: " << std::strerror(errno) << "\n";
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
        std::cerr << "[T87] getsockopt(TCP_NODELAY) failed: " << std::strerror(errno) << "\n";
        std::abort();
    }
    return value;
}

int observeHttpClientTcpNoDelay(bool tcp_no_delay)
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    LoopbackListener listener;
    HttpClient client = HttpClientBuilder().tcpNoDelay(tcp_no_delay).build();

    auto connect_result = runtime.blockOn(
        client.connect("http://127.0.0.1:" + std::to_string(listener.port()) + "/"));
    require(connect_result.has_value(), "runtime should run HttpClient connect task");
    require(connect_result.value().has_value(), "HttpClient connect should succeed against loopback listener");

    auto socket_result = client.socket();
    require(socket_result.has_value(), "HttpClient should expose connected socket");
    const int observed = readTcpNoDelay(socket_result->get().handle().fd);

    auto close_result = runtime.blockOn(client.close());
    require(close_result.has_value(), "runtime should run HttpClient close task");
    require(close_result.value().has_value(), "HttpClient close should succeed");
    runtime.stop();
    return observed;
}

#ifdef GALAY_SSL_FEATURE_ENABLED
int observeHttpsClientTcpNoDelay(bool tcp_no_delay)
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    LoopbackListener listener;
    HttpsClient client = HttpsClientBuilder().tcpNoDelay(tcp_no_delay).build();

    auto connect_result = runtime.blockOn(
        client.connect("https://127.0.0.1:" + std::to_string(listener.port()) + "/"));
    require(connect_result.has_value(), "runtime should run HttpsClient connect task");
    require(connect_result.value().has_value(), "HttpsClient connect should succeed against loopback listener");

    auto socket_result = client.socket();
    require(socket_result.has_value(), "HttpsClient should expose connected socket");
    const int observed = readTcpNoDelay(socket_result->get().handle().fd);

    auto close_result = runtime.blockOn(client.close());
    require(close_result.has_value(), "runtime should run HttpsClient close task");
    require(close_result.value().has_value(), "HttpsClient close should succeed");
    runtime.stop();
    return observed;
}
#endif

void test_builder_config_surface()
{
    auto default_http_config = HttpClientBuilder().buildConfig();
    require(default_http_config.tcp_no_delay, "HttpClientConfig should enable TCP_NODELAY by default");

    auto disabled_http_config = HttpClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_http_config.tcp_no_delay, "HttpClientBuilder should support disabling TCP_NODELAY");

#ifdef GALAY_SSL_FEATURE_ENABLED
    auto default_https_config = HttpsClientBuilder().buildConfig();
    require(default_https_config.tcp_no_delay, "HttpsClientConfig should enable TCP_NODELAY by default");

    auto disabled_https_config = HttpsClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_https_config.tcp_no_delay, "HttpsClientBuilder should support disabling TCP_NODELAY");
#endif
}

void test_plain_client_applies_config_to_connected_socket()
{
    const int default_nodelay = observeHttpClientTcpNoDelay(true);
    require(default_nodelay != 0, "default HttpClient socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeHttpClientTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled HttpClient socket should leave TCP_NODELAY off");
}

#ifdef GALAY_SSL_FEATURE_ENABLED
void test_https_client_applies_config_to_connected_socket()
{
    const int default_nodelay = observeHttpsClientTcpNoDelay(true);
    require(default_nodelay != 0, "default HttpsClient socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeHttpsClientTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled HttpsClient socket should leave TCP_NODELAY off");
}
#endif

} // namespace

int main()
{
    test_builder_config_surface();
    test_plain_client_applies_config_to_connected_socket();
#ifdef GALAY_SSL_FEATURE_ENABLED
    test_https_client_applies_config_to_connected_socket();
#endif

    std::cout << "T87-HttpClientNoDelayConfig PASS\n";
    return 0;
}
