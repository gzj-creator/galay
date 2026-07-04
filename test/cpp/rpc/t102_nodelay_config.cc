#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#define private public
#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/streamsvc.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#undef private

using namespace galay::async;
using namespace galay::kernel;
using namespace galay::rpc;

namespace {

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T102] " << message << "\n";
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
            std::cerr << "[T102] close listener failed: " << std::strerror(errno) << "\n";
        }
    }

    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    int fd() const { return m_fd; }
    uint16_t port() const { return m_port; }

private:
    int m_fd = -1;
    uint16_t m_port = 0;
};

class AcceptedTcpPair {
public:
    AcceptedTcpPair()
    {
        LoopbackListener listener;
        m_client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_client_fd < 0) {
            fail("socket failed while creating client side");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(listener.port());
        int pton_ok = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (pton_ok != 1) {
            fail("inet_pton failed while connecting pair");
        }

        if (::connect(m_client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            fail("connect failed while creating pair");
        }

        m_server_fd = ::accept(listener.fd(), nullptr, nullptr);
        if (m_server_fd < 0) {
            fail("accept failed while creating pair");
        }
    }

    ~AcceptedTcpPair()
    {
        closeClient();
        if (m_server_fd >= 0 && ::close(m_server_fd) != 0) {
            std::cerr << "[T102] close server fd failed: " << std::strerror(errno) << "\n";
        }
    }

    AcceptedTcpPair(const AcceptedTcpPair&) = delete;
    AcceptedTcpPair& operator=(const AcceptedTcpPair&) = delete;

    int clientFd() const { return m_client_fd; }
    int serverFd() const { return m_server_fd; }

    int releaseServerFd()
    {
        int fd = m_server_fd;
        m_server_fd = -1;
        return fd;
    }

    void closeClient()
    {
        if (m_client_fd >= 0) {
            if (::close(m_client_fd) != 0) {
                std::cerr << "[T102] close client fd failed: " << std::strerror(errno) << "\n";
            }
            m_client_fd = -1;
        }
    }

private:
    int m_client_fd = -1;
    int m_server_fd = -1;
};

int readTcpNoDelay(int fd)
{
    int value = 0;
    socklen_t value_len = sizeof(value);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &value_len) != 0) {
        std::cerr << "[T102] getsockopt(TCP_NODELAY) failed: " << std::strerror(errno) << "\n";
        std::abort();
    }
    return value;
}

void waitForNonBlock(int fd, const char* message)
{
    for (int i = 0; i < 100; ++i) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0 && (flags & O_NONBLOCK) != 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    fail(message);
}

int observeRpcClientTcpNoDelay(bool tcp_no_delay)
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    LoopbackListener listener;
    RpcClientConfig config = RpcClientBuilder().tcpNoDelay(tcp_no_delay).buildConfig();
    RpcClient client(config);

    auto connect_result = runtime.blockOn(client.connect("127.0.0.1", listener.port()));
    require(connect_result.has_value(), "runtime should run RpcClient connect task");
    require(connect_result.value().has_value(), "RpcClient connect should succeed against loopback listener");
    const int observed = readTcpNoDelay(client.socket().handle().fd);

    auto close_result = runtime.blockOn(client.close());
    require(close_result.has_value(), "runtime should run RpcClient close task");
    require(close_result.value().has_value(), "RpcClient close should succeed");
    runtime.stop();
    return observed;
}

int observeRpcServerAcceptedTcpNoDelay(bool tcp_no_delay)
{
    RpcServerConfig config = RpcServerBuilder().tcpNoDelay(tcp_no_delay).buildConfig();
    RpcServer server(config);
    server.m_running.store(true, std::memory_order_release);

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto start_result = runtime.start();
    require(start_result.has_value(), "runtime should start for RpcServer handleConnection probe");
    auto* scheduler = runtime.getNextIOScheduler();
    require(scheduler != nullptr, "runtime should provide an IO scheduler");

    AcceptedTcpPair pair;
    const int server_fd = pair.releaseServerFd();
    require(scheduleTask(scheduler, server.handleConnection(GHandle{server_fd})),
            "RpcServer handleConnection probe should schedule");

    waitForNonBlock(server_fd, "RpcServer handleConnection did not configure accepted socket");
    const int observed = readTcpNoDelay(server_fd);

    pair.closeClient();
    server.m_running.store(false, std::memory_order_release);
    runtime.stop();
    return observed;
}

int observeRpcStreamServerAcceptedTcpNoDelay(bool tcp_no_delay)
{
    RpcStreamServerConfig config = RpcStreamServerBuilder().tcpNoDelay(tcp_no_delay).buildConfig();
    RpcStreamServer server(config);
    server.m_running.store(true, std::memory_order_release);

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto start_result = runtime.start();
    require(start_result.has_value(), "runtime should start for RpcStreamServer handleConnection probe");
    auto* scheduler = runtime.getNextIOScheduler();
    require(scheduler != nullptr, "runtime should provide an IO scheduler");

    AcceptedTcpPair pair;
    const int server_fd = pair.releaseServerFd();
    require(scheduleTask(scheduler, server.handleConnection(GHandle{server_fd})),
            "RpcStreamServer handleConnection probe should schedule");

    waitForNonBlock(server_fd, "RpcStreamServer handleConnection did not configure accepted socket");
    const int observed = readTcpNoDelay(server_fd);

    pair.closeClient();
    server.m_running.store(false, std::memory_order_release);
    runtime.stop();
    return observed;
}

void test_builder_config_surface()
{
    auto default_server = RpcServerBuilder().buildConfig();
    require(default_server.tcp_no_delay, "RpcServerConfig should enable TCP_NODELAY by default");

    auto disabled_server = RpcServerBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_server.tcp_no_delay, "RpcServerBuilder should support disabling TCP_NODELAY");

    auto default_stream_server = RpcStreamServerBuilder().buildConfig();
    require(default_stream_server.tcp_no_delay, "RpcStreamServerConfig should enable TCP_NODELAY by default");

    auto disabled_stream_server = RpcStreamServerBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_stream_server.tcp_no_delay,
            "RpcStreamServerBuilder should support disabling TCP_NODELAY");

    auto default_client = RpcClientBuilder().buildConfig();
    require(default_client.tcp_no_delay, "RpcClientConfig should enable TCP_NODELAY by default");

    auto disabled_client = RpcClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_client.tcp_no_delay, "RpcClientBuilder should support disabling TCP_NODELAY");
}

void test_client_applies_config_to_connected_socket()
{
    const int default_nodelay = observeRpcClientTcpNoDelay(true);
    require(default_nodelay != 0, "default RpcClient socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeRpcClientTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled RpcClient socket should leave TCP_NODELAY off");
}

void test_unary_server_applies_config_to_accepted_socket()
{
    const int default_nodelay = observeRpcServerAcceptedTcpNoDelay(true);
    require(default_nodelay != 0, "default RpcServer accepted socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeRpcServerAcceptedTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled RpcServer accepted socket should leave TCP_NODELAY off");
}

void test_stream_server_applies_config_to_accepted_socket()
{
    const int default_nodelay = observeRpcStreamServerAcceptedTcpNoDelay(true);
    require(default_nodelay != 0, "default RpcStreamServer accepted socket should have TCP_NODELAY enabled");

    const int disabled_nodelay = observeRpcStreamServerAcceptedTcpNoDelay(false);
    require(disabled_nodelay == 0,
            "disabled RpcStreamServer accepted socket should leave TCP_NODELAY off");
}

} // namespace

int main()
{
    test_builder_config_surface();
    test_client_applies_config_to_connected_socket();
    test_unary_server_applies_config_to_accepted_socket();
    test_stream_server_applies_config_to_accepted_socket();

    std::cout << "T102-RpcNoDelayConfig PASS\n";
    return 0;
}
