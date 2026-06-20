#include <galay/cpp/galay-http/plugin/blacklist/blacklist.hpp>
#include <galay/cpp/galay-http/server/http_server.h>
#include <galay/cpp/galay-http/builder/http_builder.h>

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace galay::async;
using namespace galay::http;
using namespace galay::http::plugin;
using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

struct TestState {
    std::atomic<int> conn_count{0};
};

class CountingTcpPlugin final : public AcceptPlugin<TcpSocket> {
public:
    explicit CountingTcpPlugin(std::atomic<int>* calls)
        : m_calls(calls) {}

    Task<bool> handle(Runtime&, TcpSocket&, const Host&) override {
        m_calls->fetch_add(1);
        co_return true;
    }

private:
    std::atomic<int>* m_calls;
};

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T83] " << message << "\n";
    std::abort();
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
            timeval timeout{};
            timeout.tv_sec = 2;
            if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
                ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
                ::close(fd);
                fail("setsockopt timeout failed while connecting client");
            }
            return fd;
        }

        int err = errno;
        ::close(fd);
        if (err == ECONNREFUSED || err == ETIMEDOUT || err == EHOSTUNREACH || err == ENETUNREACH) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        std::cerr << "[T83] connect failed, errno=" << err << "\n";
        std::abort();
    }

    fail("connect retry exhausted");
}

bool sendAllBestEffort(int fd, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string recvAvailableUntilClosed(int fd)
{
    std::string response;
    char buffer[4096];
    while (true) {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNRESET) {
                break;
            }
            std::cerr << "[T83] recv failed, errno=" << errno
                      << " (" << std::strerror(errno) << ")\n";
            std::abort();
        }
        response.append(buffer, static_cast<size_t>(n));
    }
    return response;
}

std::string sendRawHttpBestEffort(uint16_t port)
{
    int fd = connectWithRetry(port);
    bool sent = sendAllBestEffort(
        fd,
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n"
        "\r\n");
    std::string response;
    if (sent) {
        response = recvAvailableUntilClosed(fd);
    }
    ::close(fd);
    return response;
}

Task<void> respondOk(HttpConn conn, TestState* state)
{
    state->conn_count.fetch_add(1);

    HttpRequest request;
    auto reader = conn.getReader();
    auto read_result = co_await reader.getRequest(request);
    if (!read_result) {
        co_await conn.close();
        co_return;
    }

    auto response = Http1_1ResponseBuilder()
        .status(HttpStatusCode::OK_200)
        .header("Connection", "close")
        .text("ok")
        .buildMove();

    auto writer = conn.getWriter();
    (void) co_await writer.sendResponse(response);
    co_await conn.close();
    co_return;
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

bool containsOkResponse(const std::string& response)
{
    return response.find("HTTP/1.1 200 OK") != std::string::npos;
}

struct ScenarioResult {
    std::vector<std::string> responses;
    int handled_count = 0;
};

ScenarioResult runScenario(std::unique_ptr<AcceptPlugin<TcpSocket>> plugin,
                           std::vector<std::chrono::milliseconds> delays,
                           int expected_handled_count)
{
    TestState state;

    uint16_t port = pickFreePort();
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build());

    bool registered_blacklist = server.addAcceptPlugin(std::move(plugin));
    if (!registered_blacklist) {
        fail("blacklist plugin should register before start");
    }

    server.start([&state](HttpConn conn) -> Task<void> {
        co_await respondOk(std::move(conn), &state);
    });

    ScenarioResult result;
    result.responses.reserve(delays.size());
    for (std::chrono::milliseconds delay : delays) {
        if (delay > 0ms) {
            std::this_thread::sleep_for(delay);
        }
        result.responses.push_back(sendRawHttpBestEffort(port));
    }

    if (expected_handled_count > 0) {
        waitForCount(state.conn_count, expected_handled_count,
                     "expected allowed connections did not reach handler");
    }

    server.stop();
    result.handled_count = state.conn_count.load();
    return result;
}

ScenarioResult runConcurrentScenario(std::unique_ptr<AcceptPlugin<TcpSocket>> plugin,
                                     int client_count,
                                     int io_scheduler_count,
                                     int expected_handled_count)
{
    TestState state;

    uint16_t port = pickFreePort();
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(static_cast<size_t>(io_scheduler_count))
        .computeSchedulerCount(1)
        .build());

    bool registered_blacklist = server.addAcceptPlugin(std::move(plugin));
    if (!registered_blacklist) {
        fail("blacklist plugin should register before concurrent scenario");
    }

    server.start([&state](HttpConn conn) -> Task<void> {
        co_await respondOk(std::move(conn), &state);
    });

    ScenarioResult result;
    result.responses.resize(static_cast<size_t>(client_count));

    std::atomic<int> ready_count{0};
    std::atomic<bool> start_clients{false};
    std::vector<std::thread> clients;
    clients.reserve(static_cast<size_t>(client_count));
    for (int i = 0; i < client_count; ++i) {
        clients.emplace_back([&, i]() {
            ready_count.fetch_add(1);
            while (!start_clients.load()) {
                std::this_thread::yield();
            }
            result.responses[static_cast<size_t>(i)] = sendRawHttpBestEffort(port);
        });
    }

    while (ready_count.load() < client_count) {
        std::this_thread::yield();
    }
    start_clients.store(true);

    for (std::thread& client : clients) {
        client.join();
    }

    if (expected_handled_count > 0) {
        waitForCount(state.conn_count, expected_handled_count,
                     "expected concurrent allowed connections did not reach handler");
    }

    server.stop();
    result.handled_count = state.conn_count.load();
    return result;
}

void expectResponseOk(const std::string& response, const char* message)
{
    if (!containsOkResponse(response)) {
        fail(message);
    }
}

void expectResponseBlocked(const std::string& response, const char* message)
{
    if (containsOkResponse(response)) {
        fail(message);
    }
}

void test_count_limit_compatibility()
{
    BlackList<TcpSocket>::clearConnInfo();
    ScenarioResult result = runScenario(
        std::make_unique<BlackList<TcpSocket>>(2),
        {0ms, 0ms, 0ms},
        2);

    expectResponseOk(result.responses[0], "first connection should be allowed");
    expectResponseOk(result.responses[1], "second connection should be allowed");
    expectResponseBlocked(result.responses[2],
                          "third connection should be blocked after the count limit");
    if (result.handled_count != 2) {
        fail("blocked count-limit connection should not reach the handler");
    }
    BlackList<TcpSocket>::clearConnInfo();
}

void test_interval_block_policy_unblocks_and_resets_counter()
{
    BlackList<TcpSocket>::clearConnInfo();
    BlackListConfig config;
    BlackListConfig::IntervalBlockPolicy policy;
    policy.max_attempts_per_interval = 1;
    policy.interval = 5s;
    policy.block_duration = 120ms;
    policy.reset_counter_after_unblock = true;
    config.policy = policy;

    ScenarioResult result = runScenario(
        std::make_unique<BlackList<TcpSocket>>(config),
        {0ms, 0ms, 180ms},
        2);

    expectResponseOk(result.responses[0], "first interval-policy connection should be allowed");
    expectResponseBlocked(result.responses[1],
                          "second interval-policy connection should trigger the temporary block");
    expectResponseOk(result.responses[2],
                     "connection after block_duration should be allowed after counter reset");
    if (result.handled_count != 2) {
        fail("interval policy should only deliver allowed connections to the handler");
    }
    BlackList<TcpSocket>::clearConnInfo();
}

void test_interval_block_policy_blocks_all_when_limit_is_zero()
{
    BlackList<TcpSocket>::clearConnInfo();
    BlackListConfig config;
    BlackListConfig::IntervalBlockPolicy policy;
    policy.max_attempts_per_interval = 0;
    policy.interval = 5s;
    policy.block_duration = 80ms;
    config.policy = policy;

    ScenarioResult result = runScenario(
        std::make_unique<BlackList<TcpSocket>>(config),
        {0ms, 120ms},
        0);

    expectResponseBlocked(result.responses[0],
                          "zero interval-policy limit should block the first connection");
    expectResponseBlocked(result.responses[1],
                          "zero interval-policy limit should still block after block_duration");
    if (result.handled_count != 0) {
        fail("zero interval-policy limit should not reach the handler");
    }
    BlackList<TcpSocket>::clearConnInfo();
}

void test_interval_block_policy_expires_window_without_blocking()
{
    BlackList<TcpSocket>::clearConnInfo();
    BlackListConfig config;
    BlackListConfig::IntervalBlockPolicy policy;
    policy.max_attempts_per_interval = 1;
    policy.interval = 80ms;
    policy.block_duration = 5s;
    config.policy = policy;

    ScenarioResult result = runScenario(
        std::make_unique<BlackList<TcpSocket>>(config),
        {0ms, 140ms},
        2);

    expectResponseOk(result.responses[0],
                     "first connection in an interval window should be allowed");
    expectResponseOk(result.responses[1],
                     "connection after interval expiry should start a new window");
    if (result.handled_count != 2) {
        fail("expired interval window should allow the second connection");
    }
    BlackList<TcpSocket>::clearConnInfo();
}

void test_decay_counter_policy_restores_access_after_decay()
{
    BlackList<TcpSocket>::clearConnInfo();
    BlackListConfig config;
    BlackListConfig::DecayCounterPolicy policy;
    policy.max_attempts = 2;
    policy.decay_interval = 80ms;
    policy.decay_step = 2;
    policy.max_counter_value = 8;
    config.policy = policy;

    ScenarioResult result = runScenario(
        std::make_unique<BlackList<TcpSocket>>(config),
        {0ms, 0ms, 0ms, 140ms},
        3);

    expectResponseOk(result.responses[0], "first decay-policy connection should be allowed");
    expectResponseOk(result.responses[1], "second decay-policy connection should be allowed");
    expectResponseBlocked(result.responses[2],
                          "third decay-policy connection should be blocked before decay");
    expectResponseOk(result.responses[3],
                     "connection after enough decay should be allowed again");
    if (result.handled_count != 3) {
        fail("decay policy should only deliver allowed connections to the handler");
    }
    BlackList<TcpSocket>::clearConnInfo();
}

void test_decay_counter_policy_does_not_decay_before_full_interval()
{
    BlackList<TcpSocket>::clearConnInfo();
    BlackListConfig config;
    BlackListConfig::DecayCounterPolicy policy;
    policy.max_attempts = 1;
    policy.decay_interval = 160ms;
    policy.decay_step = 1;
    policy.max_counter_value = 4;
    config.policy = policy;

    ScenarioResult result = runScenario(
        std::make_unique<BlackList<TcpSocket>>(config),
        {0ms, 70ms},
        1);

    expectResponseOk(result.responses[0],
                     "first decay boundary connection should be allowed");
    expectResponseBlocked(result.responses[1],
                          "decay should not restore access before a full decay interval");
    if (result.handled_count != 1) {
        fail("pre-interval decay boundary should only deliver one connection");
    }
    BlackList<TcpSocket>::clearConnInfo();
}

void test_separate_plugin_instances_share_blacklist_state()
{
    BlackList<TcpSocket>::clearConnInfo();

    BlackListConfig config;
    BlackListConfig::IntervalBlockPolicy policy;
    policy.max_attempts_per_interval = 1;
    policy.interval = 5s;
    policy.block_duration = 5s;
    config.policy = policy;

    ScenarioResult first = runScenario(
        std::make_unique<BlackList<TcpSocket>>(config),
        {0ms},
        1);
    ScenarioResult second = runScenario(
        std::make_unique<BlackList<TcpSocket>>(config),
        {0ms},
        0);

    expectResponseOk(first.responses[0],
                     "first plugin instance should allow the first connection");
    expectResponseBlocked(second.responses[0],
                          "second plugin instance should share blacklist state and block");
    if (first.handled_count != 1 || second.handled_count != 0) {
        fail("shared blacklist state should only deliver the first connection");
    }
    BlackList<TcpSocket>::clearConnInfo();
}

void test_concurrent_pressure_shares_limit_across_server_loops()
{
    BlackList<TcpSocket>::clearConnInfo();

    constexpr int allowed_limit = 8;
    constexpr int client_count = 32;

    BlackListConfig config;
    BlackListConfig::IntervalBlockPolicy policy;
    policy.max_attempts_per_interval = allowed_limit;
    policy.interval = 30s;
    policy.block_duration = 30s;
    config.policy = policy;

    ScenarioResult result = runConcurrentScenario(
        std::make_unique<BlackList<TcpSocket>>(config),
        client_count,
        4,
        allowed_limit);

    int ok_count = static_cast<int>(std::count_if(
        result.responses.begin(), result.responses.end(), containsOkResponse));
    if (ok_count != allowed_limit) {
        fail("concurrent pressure should allow exactly the configured limit");
    }
    if (result.handled_count != allowed_limit) {
        fail("concurrent pressure should deliver exactly the allowed connections");
    }
    BlackList<TcpSocket>::clearConnInfo();
}

void test_decay_counter_policy_concurrent_pressure_shares_limit_across_server_loops()
{
    BlackList<TcpSocket>::clearConnInfo();

    constexpr int allowed_limit = 8;
    constexpr int client_count = 32;

    BlackListConfig config;
    BlackListConfig::DecayCounterPolicy policy;
    policy.max_attempts = allowed_limit;
    policy.decay_interval = 30s;
    policy.decay_step = 1;
    policy.max_counter_value = allowed_limit + 1;
    config.policy = policy;

    ScenarioResult result = runConcurrentScenario(
        std::make_unique<BlackList<TcpSocket>>(config),
        client_count,
        4,
        allowed_limit);

    int ok_count = static_cast<int>(std::count_if(
        result.responses.begin(), result.responses.end(), containsOkResponse));
    if (ok_count != allowed_limit) {
        fail("concurrent decay policy should allow exactly the configured limit");
    }
    if (result.handled_count != allowed_limit) {
        fail("concurrent decay policy should deliver exactly the allowed connections");
    }
    BlackList<TcpSocket>::clearConnInfo();
}

void test_excluded_ip_bypasses_blacklist_but_continues_plugin_chain()
{
    BlackList<TcpSocket>::clearConnInfo();

    TestState state;
    std::atomic<int> downstream_count{0};

    BlackListConfig config;
    BlackListConfig::IntervalBlockPolicy policy;
    policy.max_attempts_per_interval = 0;
    policy.interval = 5s;
    policy.block_duration = 5s;
    config.policy = policy;
    config.exclude_ips.insert("127.0.0.1");

    uint16_t port = pickFreePort();
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build());

    bool registered_blacklist = server.addAcceptPlugin(std::make_unique<BlackList<TcpSocket>>(config));
    bool registered_downstream = server.addAcceptPlugin(
        std::make_unique<CountingTcpPlugin>(&downstream_count));
    if (!registered_blacklist || !registered_downstream) {
        fail("excluded-ip blacklist and downstream plugins should register before start");
    }

    server.start([&state](HttpConn conn) -> Task<void> {
        co_await respondOk(std::move(conn), &state);
    });

    std::string first_response = sendRawHttpBestEffort(port);
    std::string second_response = sendRawHttpBestEffort(port);

    waitForCount(state.conn_count, 2,
                 "excluded IP connections should both reach handler");
    server.stop();

    expectResponseOk(first_response,
                     "excluded IP should bypass zero-limit blacklist on first connection");
    expectResponseOk(second_response,
                     "excluded IP should bypass zero-limit blacklist on repeated connection");
    if (downstream_count.load() != 2) {
        fail("excluded IP should continue downstream accept plugins");
    }
    if (state.conn_count.load() != 2) {
        fail("excluded IP should continue business handling");
    }
    BlackList<TcpSocket>::clearConnInfo();
}

void test_blacklist_stops_downstream_accept_plugin_and_handler()
{
    BlackList<TcpSocket>::clearConnInfo();

    TestState state;
    std::atomic<int> downstream_count{0};

    BlackListConfig config;
    BlackListConfig::IntervalBlockPolicy policy;
    policy.max_attempts_per_interval = 1;
    policy.interval = 5s;
    policy.block_duration = 5s;
    config.policy = policy;

    uint16_t port = pickFreePort();
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build());

    bool registered_blacklist = server.addAcceptPlugin(std::make_unique<BlackList<TcpSocket>>(config));
    bool registered_downstream = server.addAcceptPlugin(
        std::make_unique<CountingTcpPlugin>(&downstream_count));
    if (!registered_blacklist || !registered_downstream) {
        fail("blacklist and downstream accept plugins should register before start");
    }

    server.start([&state](HttpConn conn) -> Task<void> {
        co_await respondOk(std::move(conn), &state);
    });

    std::string first_response = sendRawHttpBestEffort(port);
    std::string second_response = sendRawHttpBestEffort(port);

    waitForCount(state.conn_count, 1, "first plugin-chain connection should reach handler");
    server.stop();

    expectResponseOk(first_response, "first plugin-chain connection should be allowed");
    expectResponseBlocked(second_response,
                          "second plugin-chain connection should be blocked by blacklist");
    if (downstream_count.load() != 1) {
        fail("blacklist should stop downstream accept plugins after blocking");
    }
    if (state.conn_count.load() != 1) {
        fail("blacklist should stop handler after blocking");
    }
    BlackList<TcpSocket>::clearConnInfo();
}

} // namespace

int main()
{
    test_count_limit_compatibility();
    test_interval_block_policy_unblocks_and_resets_counter();
    test_interval_block_policy_blocks_all_when_limit_is_zero();
    test_interval_block_policy_expires_window_without_blocking();
    test_decay_counter_policy_restores_access_after_decay();
    test_decay_counter_policy_does_not_decay_before_full_interval();
    test_separate_plugin_instances_share_blacklist_state();
    test_concurrent_pressure_shares_limit_across_server_loops();
    test_decay_counter_policy_concurrent_pressure_shares_limit_across_server_loops();
    test_excluded_ip_bypasses_blacklist_but_continues_plugin_chain();
    test_blacklist_stops_downstream_accept_plugin_and_handler();

    std::cout << "T83-BlackList PASS\n";
    return 0;
}
