#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#define private public
#include <galay/cpp/galay-redis/async/redis_client.h>
#include <galay/cpp/galay-redis/protoc/connection.h>
#include <galay/cpp/galay-redis/sync/redis_session.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#undef private

using namespace galay::kernel;
using namespace galay::redis;

namespace {

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T26] " << message << "\n";
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
            std::cerr << "[T26] close listener failed: " << std::strerror(errno) << "\n";
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
        std::cerr << "[T26] getsockopt(TCP_NODELAY) failed: " << std::strerror(errno) << "\n";
        std::abort();
    }
    return value;
}

Task<RedisVoidResult> connectRedisClient(RedisClient* client, uint16_t port)
{
    if (client == nullptr) {
        co_return std::unexpected(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
            "client is null"));
    }
    co_return co_await client->connect("127.0.0.1", port);
}

Task<std::expected<void, IOError>> closeRedisClient(RedisClient* client)
{
    if (client == nullptr) {
        co_return std::unexpected(IOError(kNotReady, 0));
    }
    co_return co_await client->close();
}

int observeProtocolConnectionTcpNoDelay(bool tcp_no_delay)
{
    LoopbackListener listener;
    protocol::Connection connection;
    auto connect_result = connection.connect("127.0.0.1", listener.port(), 5000, tcp_no_delay);
    require(connect_result.has_value(), "protocol Connection should connect to loopback listener");
    const int observed = readTcpNoDelay(connection.m_socket_fd);
    connection.disconnect();
    return observed;
}

int observeRedisSessionTcpNoDelay(bool tcp_no_delay)
{
    LoopbackListener listener;
    RedisSessionConfig config;
    config.host = "127.0.0.1";
    config.port = listener.port();
    config.tcp_no_delay = tcp_no_delay;
    RedisSession session(config);

    auto connect_result = session.connect();
    require(connect_result.has_value(), "RedisSession should connect to loopback listener");
    require(session.m_connection != nullptr, "RedisSession should own a protocol connection");
    const int observed = readTcpNoDelay(session.m_connection->m_socket_fd);
    auto disconnect_result = session.disconnect();
    require(disconnect_result.has_value(), "RedisSession disconnect should succeed");
    return observed;
}

int observeAsyncRedisClientTcpNoDelay(bool tcp_no_delay)
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto start_result = runtime.start();
    require(start_result.has_value(), "runtime should start for RedisClient connect probe");
    auto* scheduler = runtime.getNextIOScheduler();
    require(scheduler != nullptr, "runtime should provide an IO scheduler");

    LoopbackListener listener;
    auto config = RedisClientBuilder()
        .scheduler(scheduler)
        .tcpNoDelay(tcp_no_delay)
        .buildConfig();
    RedisClient client(scheduler, config);

    auto connect_result = runtime.blockOn(connectRedisClient(&client, listener.port()));
    require(connect_result.has_value(), "runtime should run RedisClient connect task");
    require(connect_result.value().has_value(), "RedisClient connect should succeed against loopback listener");
    const int observed = readTcpNoDelay(client.socket().handle().fd);

    auto close_result = runtime.blockOn(closeRedisClient(&client));
    require(close_result.has_value(), "runtime should run RedisClient close task");
    require(close_result.value().has_value(), "RedisClient close should succeed");
    runtime.stop();
    return observed;
}

void test_config_surface()
{
    RedisSessionConfig sync_config;
    require(sync_config.tcp_no_delay, "RedisSessionConfig should enable TCP_NODELAY by default");

    AsyncRedisConfig async_config;
    require(async_config.tcp_no_delay, "AsyncRedisConfig should enable TCP_NODELAY by default");

    auto disabled_async = RedisClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_async.tcp_no_delay, "RedisClientBuilder should support disabling TCP_NODELAY");

#ifdef GALAY_SSL_FEATURE_ENABLED
    auto disabled_rediss = RedissClientBuilder().tcpNoDelay(false).buildConfig();
    require(!disabled_rediss.tcp_no_delay, "RedissClientBuilder should support disabling TCP_NODELAY");
#endif
}

void test_protocol_connection_applies_config()
{
    const int default_nodelay = observeProtocolConnectionTcpNoDelay(true);
    require(default_nodelay != 0, "default Redis protocol Connection should enable TCP_NODELAY");

    const int disabled_nodelay = observeProtocolConnectionTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled Redis protocol Connection should leave TCP_NODELAY off");
}

void test_sync_session_applies_config()
{
    const int default_nodelay = observeRedisSessionTcpNoDelay(true);
    require(default_nodelay != 0, "default RedisSession should enable TCP_NODELAY");

    const int disabled_nodelay = observeRedisSessionTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled RedisSession should leave TCP_NODELAY off");
}

void test_async_client_applies_config()
{
    const int default_nodelay = observeAsyncRedisClientTcpNoDelay(true);
    require(default_nodelay != 0, "default RedisClient should enable TCP_NODELAY");

    const int disabled_nodelay = observeAsyncRedisClientTcpNoDelay(false);
    require(disabled_nodelay == 0, "disabled RedisClient should leave TCP_NODELAY off");
}

} // namespace

int main()
{
    test_config_surface();
    test_protocol_connection_applies_config();
    test_sync_session_applies_config();
    test_async_client_applies_config();

    std::cout << "T26-RedisNoDelayConfig PASS\n";
    return 0;
}
