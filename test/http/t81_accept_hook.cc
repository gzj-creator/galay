#include "galay-http/server/http_server.h"
#include "galay-http/builder/http_builder.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace galay::async;
using namespace galay::http;
using namespace galay::kernel;

namespace {

struct TestState {
    std::atomic<int> first_count{0};
    std::atomic<int> second_count{0};
    std::atomic<int> conn_count{0};
    std::atomic<int> ordered_second_count{0};
    std::atomic<int> valid_socket_count{0};
    std::atomic<int> first_start_count{0};
    std::atomic<int> second_start_count{0};
    std::atomic<int> first_stop_count{0};
    std::atomic<int> second_stop_count{0};
};

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T81] " << message << "\n";
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
            timeout.tv_sec = 5;
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

        std::cerr << "[T81] connect failed, errno=" << err << "\n";
        std::abort();
    }

    fail("connect retry exhausted");
}

void sendAll(int fd, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            std::cerr << "[T81] send failed, errno=" << errno
                      << " (" << std::strerror(errno) << ")\n";
            std::abort();
        }
        sent += static_cast<size_t>(n);
    }
}

std::string recvUntilClosed(int fd)
{
    std::string response;
    char buffer[4096];
    while (true) {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            std::cerr << "[T81] recv failed, errno=" << errno
                      << " (" << std::strerror(errno) << ")\n";
            std::abort();
        }
        response.append(buffer, static_cast<size_t>(n));
    }
    return response;
}

std::string sendRawHttp(uint16_t port)
{
    int fd = connectWithRetry(port);
    sendAll(fd,
            "GET / HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n"
            "\r\n");
    std::string response = recvUntilClosed(fd);
    ::close(fd);
    return response;
}

Task<bool> firstAcceptHook(TcpSocket& socket, const Host&, TestState* state)
{
    state->first_count.fetch_add(1);
    if (socket.handle().fd >= 0) {
        state->valid_socket_count.fetch_add(1);
    }

    co_return true;
}

Task<bool> secondAcceptHook(TcpSocket&, const Host&, TestState* state)
{
    int prior_second_count = state->second_count.load();
    if (state->first_count.load() > prior_second_count) {
        state->ordered_second_count.fetch_add(1);
    }
    state->second_count.fetch_add(1);

    co_return true;
}

class FirstPlugin final : public plugin::AcceptPlugin<TcpSocket> {
public:
    explicit FirstPlugin(TestState* state)
        : m_state(state) {}

    bool start(Runtime&) override {
        m_state->first_start_count.fetch_add(1);
        return true;
    }

    void stop() noexcept override {
        m_state->first_stop_count.fetch_add(1);
    }

    Task<bool> handle(Runtime&, TcpSocket& socket, const Host& client_host) override {
        auto continuing = co_await firstAcceptHook(socket, client_host, m_state);
        co_return continuing.value_or(false);
    }

private:
    TestState* m_state;
};

class SecondPlugin final : public plugin::AcceptPlugin<TcpSocket> {
public:
    explicit SecondPlugin(TestState* state)
        : m_state(state) {}

    bool start(Runtime&) override {
        m_state->second_start_count.fetch_add(1);
        return true;
    }

    void stop() noexcept override {
        m_state->second_stop_count.fetch_add(1);
    }

    Task<bool> handle(Runtime&, TcpSocket& socket, const Host& client_host) override {
        auto continuing = co_await secondAcceptHook(socket, client_host, m_state);
        co_return continuing.value_or(false);
    }

private:
    TestState* m_state;
};

class FailingStartPlugin final : public plugin::AcceptPlugin<TcpSocket> {
public:
    explicit FailingStartPlugin(std::atomic<int>* stop_count)
        : m_stop_count(stop_count) {}

    bool start(Runtime&) override {
        return false;
    }

    void stop() noexcept override {
        m_stop_count->fetch_add(1);
    }

    Task<bool> handle(Runtime&, TcpSocket&, const Host&) override {
        co_return true;
    }

private:
    std::atomic<int>* m_stop_count;
};

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

void test_start_failure_stops_already_started_plugins()
{
    TestState state;
    std::atomic<int> failing_stop_count{0};

    uint16_t port = pickFreePort();
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build());

    bool registered_first = server.addAcceptPlugin(std::make_unique<FirstPlugin>(&state));
    bool registered_failing =
        server.addAcceptPlugin(std::make_unique<FailingStartPlugin>(&failing_stop_count));
    if (!registered_first || !registered_failing) {
        fail("start-failure plugins should register before start");
    }

    server.start([&state](HttpConn conn) -> Task<void> {
        co_await respondOk(std::move(conn), &state);
    });

    if (server.isRunning()) {
        fail("server should not be running after accept plugin start failure");
    }
    waitForCount(state.first_start_count, 1, "first plugin start should run before failure");
    waitForCount(state.first_stop_count, 1, "started plugin stop should run after failure");
    if (failing_stop_count.load() != 0) {
        fail("failing plugin stop should not run when start returned false before being counted");
    }
}

} // namespace

int main()
{
    test_start_failure_stops_already_started_plugins();

    TestState state;

    uint16_t port = pickFreePort();
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build());

    if (server.addAcceptPlugin(nullptr)) {
        fail("null plugin registration should be rejected");
    }

    bool registered_first = server.addAcceptPlugin(std::make_unique<FirstPlugin>(&state));
    bool registered_second = server.addAcceptPlugin(std::make_unique<SecondPlugin>(&state));

    if (!registered_first || !registered_second) {
        fail("accept handlers should register before start");
    }

    server.start([&state](HttpConn conn) -> Task<void> {
        co_await respondOk(std::move(conn), &state);
    });

    bool registered_after_start = server.addAcceptPlugin(
        std::make_unique<FirstPlugin>(&state));
    if (registered_after_start) {
        fail("accept handler registration after start should be rejected");
    }

    std::string first_response = sendRawHttp(port);
    std::string second_response = sendRawHttp(port);

    server.stop();

    if (first_response.find("HTTP/1.1 200 OK") == std::string::npos ||
        second_response.find("HTTP/1.1 200 OK") == std::string::npos) {
        fail("server should respond after accept hooks run");
    }

    waitForCount(state.first_count, 2, "first hook did not run twice");
    waitForCount(state.second_count, 2, "second hook did not run twice");
    waitForCount(state.conn_count, 2, "connection handler did not run twice");
    waitForCount(state.first_start_count, 1, "first plugin start should run once");
    waitForCount(state.second_start_count, 1, "second plugin start should run once");
    waitForCount(state.first_stop_count, 1, "first plugin stop should run once");
    waitForCount(state.second_stop_count, 1, "second plugin stop should run once");

    if (state.valid_socket_count.load() != 2) {
        fail("accept hook should receive a valid TcpSocket");
    }
    if (state.ordered_second_count.load() != 2) {
        fail("second hook should run after first hook for each accept");
    }

    std::cout << "T81-AcceptHook PASS\n";
    return 0;
}
