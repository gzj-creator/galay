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

#define private public
#include <galay/cpp/galay-mysql/async/client.h>
#include <galay/cpp/galay-mysql/sync/mysql_client.h>
#undef private

using namespace galay::mysql;

namespace {

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T19] " << message << "\n";
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
            std::cerr << "[T19] close listener failed: " << std::strerror(errno) << "\n";
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
        std::cerr << "[T19] getsockopt(TCP_NODELAY) failed: " << std::strerror(errno) << "\n";
        std::abort();
    }
    return value;
}

int observeSyncConnectSocketTcpNoDelay(bool tcp_no_delay)
{
    LoopbackListener listener;
    MysqlClient client;
    auto connect_result = client.connectSocket("127.0.0.1", listener.port(), 5000, tcp_no_delay);
    require(connect_result.has_value(), "MysqlClient connectSocket should connect to loopback listener");
    const int observed = readTcpNoDelay(client.m_socket_fd);
    client.close();
    return observed;
}

int observeAsyncConnectSetupTcpNoDelay(bool tcp_no_delay)
{
    AsyncMysqlClient client(nullptr);
    MysqlConfig config = MysqlConfig::defaultConfig();
    config.tcp_no_delay = tcp_no_delay;
    MysqlConnectAwaitable::SharedState state(client, config);
    require(!state.result.has_value(), "MysqlConnectAwaitable setup should remain ready to connect");
    return readTcpNoDelay(client.socket().handle().fd);
}

void test_config_surface()
{
    MysqlConfig config = MysqlConfig::defaultConfig();
    require(config.tcp_no_delay, "MysqlConfig should enable TCP_NODELAY by default");

    config.tcp_no_delay = false;
    require(!config.tcp_no_delay, "MysqlConfig should support disabling TCP_NODELAY");

    MysqlConfig created = MysqlConfig::create("127.0.0.1", 3306, "root", "secret");
    require(created.tcp_no_delay, "MysqlConfig::create should preserve the default TCP_NODELAY policy");

    AsyncMysqlConfig async_config;
    require(async_config.tcp_no_delay, "AsyncMysqlConfig should enable TCP_NODELAY by default");

    auto disabled_async = AsyncMysqlClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_async.tcp_no_delay, "AsyncMysqlClientBuilder should support disabling TCP_NODELAY");
}

void test_sync_client_applies_config()
{
    const int default_nodelay = observeSyncConnectSocketTcpNoDelay(true);
    require(default_nodelay != 0, "default MysqlClient socket should enable TCP_NODELAY");

    const int disabled_nodelay = observeSyncConnectSocketTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled MysqlClient socket should leave TCP_NODELAY off");
}

void test_async_client_applies_config()
{
    const int default_nodelay = observeAsyncConnectSetupTcpNoDelay(true);
    require(default_nodelay != 0, "default AsyncMysqlClient connect setup should enable TCP_NODELAY");

    const int disabled_nodelay = observeAsyncConnectSetupTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled AsyncMysqlClient connect setup should leave TCP_NODELAY off");
}

} // namespace

int main()
{
    test_config_surface();
    test_sync_client_applies_config();
    test_async_client_applies_config();

    std::cout << "T19-MySQLNoDelayConfig PASS\n";
    return 0;
}
