#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

#include <galay/cpp/galay-kernel/core/runtime.h>

#define private public
#include <galay/cpp/galay-etcd/async/client.h>
#include <galay/cpp/galay-etcd/sync/etcd_client.h>
#undef private

using namespace galay::etcd;
using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;

namespace {

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T16] " << message << "\n";
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
            std::cerr << "[T16] close listener failed: " << std::strerror(errno) << "\n";
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
        std::cerr << "[T16] getsockopt(TCP_NODELAY) failed: " << std::strerror(errno) << "\n";
        std::abort();
    }
    return value;
}

std::string endpointFor(uint16_t port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

int observeSyncClientTcpNoDelay(bool tcp_no_delay)
{
    LoopbackListener listener;
    EtcdConfig config = EtcdClientBuilder()
        .endpoint(endpointFor(listener.port()))
        .tcpNoDelay(tcp_no_delay)
        .buildConfig();
    EtcdClient client(config);

    auto connect_result = client.connect();
    require(connect_result.has_value(), "EtcdClient should connect to loopback listener");
    const int observed = readTcpNoDelay(client.m_socket_fd);
    auto close_result = client.close();
    require(close_result.has_value(), "EtcdClient close should succeed");
    return observed;
}

int observeAsyncConnectSetupTcpNoDelay(bool tcp_no_delay)
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto start_result = runtime.start();
    require(start_result.has_value(), "runtime should start for AsyncEtcdClient connect probe");
    auto* scheduler = runtime.getNextIOScheduler();
    require(scheduler != nullptr, "runtime should provide an IO scheduler");

    EtcdConfig config = AsyncEtcdClientBuilder()
        .scheduler(scheduler)
        .endpoint("http://127.0.0.1:2379")
        .tcpNoDelay(tcp_no_delay)
        .buildConfig();
    AsyncEtcdClient client(scheduler, config);
    AsyncEtcdClient::ConnectAwaitable::SharedState state(client);
    require(!state.result.has_value(), "AsyncEtcdClient setup should remain ready to connect");
    require(client.m_socket != nullptr, "AsyncEtcdClient setup should create a socket");
    const int observed = readTcpNoDelay(client.m_socket->handle().fd);
    runtime.stop();
    return observed;
}

void test_config_surface()
{
    EtcdNetworkConfig network_config;
    require(network_config.tcp_no_delay, "EtcdNetworkConfig should enable TCP_NODELAY by default");

    EtcdConfig etcd_config;
    require(etcd_config.tcp_no_delay, "EtcdConfig should inherit TCP_NODELAY default");

    auto disabled_sync = EtcdClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_sync.tcp_no_delay, "EtcdClientBuilder should support disabling TCP_NODELAY");

    auto disabled_async = AsyncEtcdClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_async.tcp_no_delay, "AsyncEtcdClientBuilder should support disabling TCP_NODELAY");
}

void test_sync_client_applies_config()
{
    const int default_nodelay = observeSyncClientTcpNoDelay(true);
    require(default_nodelay != 0, "default EtcdClient socket should enable TCP_NODELAY");

    const int disabled_nodelay = observeSyncClientTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled EtcdClient socket should leave TCP_NODELAY off");
}

void test_async_client_applies_config()
{
    const int default_nodelay = observeAsyncConnectSetupTcpNoDelay(true);
    require(default_nodelay != 0, "default AsyncEtcdClient setup should enable TCP_NODELAY");

    const int disabled_nodelay = observeAsyncConnectSetupTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled AsyncEtcdClient setup should leave TCP_NODELAY off");
}

} // namespace

int main()
{
    test_config_surface();
    test_sync_client_applies_config();
    test_async_client_applies_config();

    std::cout << "T16-EtcdNoDelayConfig PASS\n";
    return 0;
}
