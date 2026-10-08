#include <galay/cpp/galay-http/server/http_policy.h>
#include <galay/cpp/galay-http/server/http_router.h>
#include <galay/cpp/galay-http/server/http_server.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

using namespace galay::http;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

[[noreturn]] void fail(const std::string& message);

class SetSendBufferPlugin final : public plugin::AcceptPlugin<AsyncTcpSocket> {
public:
    explicit SetSendBufferPlugin(int size)
        : m_size(size) {}

    Task<bool> handle(Runtime&, AsyncTcpSocket& socket, const Host&) override
    {
        if (::setsockopt(socket.handle().fd, SOL_SOCKET, SO_SNDBUF, &m_size, sizeof(m_size)) != 0) {
            fail("setsockopt SO_SNDBUF failed in accept plugin, errno=" + std::to_string(errno));
        }
        co_return true;
    }

private:
    int m_size;
};

volatile sig_atomic_t g_stage = 0;
std::atomic<int> g_request_count{0};
std::atomic<bool> g_slow_send_finished{false};
std::atomic<int> g_slow_send_result{0};
std::atomic<bool> g_static_sendfile_finished{false};

void alarm_handler(int)
{
    std::cerr << "[T85] timeout at stage " << g_stage << "\n";
    ::_exit(2);
}

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "[T85] " << message << "\n";
    std::abort();
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

void close_fd(int fd, const char* context)
{
    if (::close(fd) != 0) {
        fail(std::string(context) + ": close failed, errno=" + std::to_string(errno));
    }
}

uint16_t pick_free_port()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("socket failed while picking a free port");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        close_fd(fd, "pickFreePort inet_pton");
        fail("inet_pton failed while picking a free port");
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_fd(fd, "pickFreePort bind");
        fail("bind failed while picking a free port");
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        close_fd(fd, "pickFreePort getsockname");
        fail("getsockname failed while picking a free port");
    }

    const uint16_t port = ntohs(addr.sin_port);
    close_fd(fd, "pickFreePort success");
    return port;
}

int connect_with_retry(uint16_t port, std::chrono::milliseconds recv_timeout)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        fail("inet_pton failed while connecting client");
    }

    for (int attempt = 0; attempt < 100; ++attempt) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            fail("socket failed while connecting client");
        }

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            timeval timeout{};
            timeout.tv_sec = static_cast<time_t>(recv_timeout.count() / 1000);
            timeout.tv_usec = static_cast<suseconds_t>((recv_timeout.count() % 1000) * 1000);
            if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
                ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
                close_fd(fd, "connectWithRetry setsockopt");
                fail("setsockopt timeout failed while connecting client");
            }
            return fd;
        }

        const int err = errno;
        close_fd(fd, "connectWithRetry failed attempt");
        if (err == ECONNREFUSED || err == ETIMEDOUT || err == EHOSTUNREACH || err == ENETUNREACH) {
            std::this_thread::sleep_for(20ms);
            continue;
        }

        fail("connect failed, errno=" + std::to_string(err));
    }

    fail("connect retry exhausted");
}

void send_all(int fd, std::string_view data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            fail("send failed, errno=" + std::to_string(errno));
        }
        sent += static_cast<size_t>(n);
    }
}

std::string recv_once(int fd)
{
    char buffer[4096];
    ssize_t n = 0;
    do {
        n = ::recv(fd, buffer, sizeof(buffer), 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        fail("recv failed at stage " + std::to_string(g_stage) + ", errno=" + std::to_string(errno));
    }
    if (n == 0) {
        return {};
    }
    return std::string(buffer, static_cast<size_t>(n));
}

void set_socket_receive_buffer(int fd, int size)
{
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0) {
        fail("setsockopt SO_RCVBUF failed, errno=" + std::to_string(errno));
    }
}

void assert_contains(const std::string& text, const std::string& expected)
{
    if (text.find(expected) == std::string::npos) {
        std::cerr << "[T85] expected substring not found: " << expected << "\n";
        std::cerr << "[T85] actual payload:\n" << text << "\n";
        std::abort();
    }
}

void create_large_file(const std::string& path, size_t size)
{
    int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fail("open sparse file for static sendfile timeout test failed, errno=" + std::to_string(errno));
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close_fd(fd, "createLargeFile ftruncate");
        fail("ftruncate sparse file for static sendfile timeout test failed, errno=" + std::to_string(errno));
    }
    close_fd(fd, "createLargeFile success");
}

Task<void> ok_handler(HttpConn& conn, HttpRequest request)
{
    const int request_no = g_request_count.fetch_add(1) + 1;
    auto response = Http1_1ResponseBuilder::ok()
        .header("Content-Type", "text/plain")
        .header("X-Request-Count", std::to_string(request_no))
        .header("Connection", request.header().is_keep_alive() ? "keep-alive" : "close")
        .text("ok")
        .build_move();
    auto writer = conn.get_writer();
    auto result = co_await writer.send_response(response);
    if (!result) {
        fail("ok handler send failed: " + result.error().message());
    }
    co_return;
}

Task<void> slow_response_handler(HttpConn& conn, HttpRequest)
{
    constexpr size_t kSlowResponseBodySize = 16 * 1024 * 1024;
    int send_buffer_size = 1024;
    if (::setsockopt(conn.get_socket().handle().fd,
                     SOL_SOCKET,
                     SO_SNDBUF,
                     &send_buffer_size,
                     sizeof(send_buffer_size)) != 0) {
        fail("setsockopt SO_SNDBUF failed in slowResponseHandler, errno=" + std::to_string(errno));
    }

    std::string body(kSlowResponseBodySize, 'x');
    auto response = Http1_1ResponseBuilder::ok()
        .header("Content-Type", "text/plain")
        .header("Connection", "close")
        .body(std::move(body))
        .build_move();
    auto writer = conn.get_writer();
    auto result = co_await writer.send_response(response);
    g_slow_send_result.store(result ? 1 : -static_cast<int>(result.error().code()));
    g_slow_send_finished.store(true);
    co_return;
}

HttpServer make_server(uint16_t port, HttpServerPolicy policy)
{
    return HttpServer(HttpServerBuilder<>()
        .host("127.0.0.1")
        .port(port)
        .io_scheduler_count(1)
        .parallel_scheduler_count(1)
        .policy(std::move(policy))
        .build_config());
}

HttpRouter make_router()
{
    HttpRouter router;
    router.add_handler<HttpMethod::GET, HttpMethod::POST>("/ok", ok_handler);
    router.add_handler<HttpMethod::GET>("/slow", slow_response_handler);
    return router;
}

void verify_initial_request_timeout_returns408()
{
    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 200ms;
    policy.keep_alive.keep_alive_idle_timeout = 200ms;

    const uint16_t port = pick_free_port();
    auto server = make_server(port, policy);

    g_stage = 1;
    server.start(make_router());

    g_stage = 2;
    int fd = connect_with_retry(port, 1500ms);
    std::string response = recv_once(fd);
    close_fd(fd, "verifyInitialRequestTimeoutReturns408");

    assert_contains(response, "HTTP/1.1 408 Request Timeout");
    assert_contains(response, "connection: close");

    g_stage = 3;
    server.stop();
}

void verify_keep_alive_idle_timeout_closes_before_next_request()
{
    g_request_count.store(0);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 300ms;
    policy.keep_alive.keep_alive_idle_timeout = 200ms;

    const uint16_t port = pick_free_port();
    auto server = make_server(port, policy);

    g_stage = 4;
    server.start(make_router());

    g_stage = 5;
    int fd = connect_with_retry(port, 1500ms);
    send_all(fd,
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
    std::string first_response = recv_once(fd);
    assert_contains(first_response, "HTTP/1.1 200 OK");
    assert_contains(first_response, "x-request-count: 1");

    std::this_thread::sleep_for(350ms);

    g_stage = 6;
    send_all(fd,
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");
    std::string second_response = recv_once(fd);
    close_fd(fd, "verifyKeepAliveIdleTimeoutClosesBeforeNextRequest");

    require(second_response.empty(),
            "idle keep-alive connection should be closed before second request");
    require(g_request_count.load() == 1,
            "server should not dispatch second request after idle timeout");

    g_stage = 7;
    server.stop();
}

void verify_initial_request_body_timeout_returns408()
{
    g_request_count.store(0);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 1s;
    policy.timeouts.request_body_timeout = 200ms;
    policy.keep_alive.keep_alive_idle_timeout = 1s;

    const uint16_t port = pick_free_port();
    auto server = make_server(port, policy);

    g_stage = 8;
    server.start(make_router());

    g_stage = 9;
    int fd = connect_with_retry(port, 700ms);
    send_all(fd,
            "POST /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 4\r\n"
            "Connection: close\r\n"
            "\r\n");
    std::string response = recv_once(fd);
    close_fd(fd, "verifyInitialRequestBodyTimeoutReturns408");

    assert_contains(response, "HTTP/1.1 408 Request Timeout");
    assert_contains(response, "connection: close");
    require(g_request_count.load() == 0,
            "server should not dispatch request when body times out");

    g_stage = 10;
    server.stop();
}

void verify_keep_alive_second_request_slow_header_closes_connection()
{
    g_request_count.store(0);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 1s;
    policy.keep_alive.keep_alive_idle_timeout = 200ms;

    const uint16_t port = pick_free_port();
    auto server = make_server(port, policy);

    g_stage = 11;
    server.start(make_router());

    g_stage = 12;
    int fd = connect_with_retry(port, 700ms);
    send_all(fd,
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
    std::string first_response = recv_once(fd);
    assert_contains(first_response, "HTTP/1.1 200 OK");
    assert_contains(first_response, "x-request-count: 1");

    send_all(fd,
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n");
    std::string second_response = recv_once(fd);
    close_fd(fd, "verifyKeepAliveSecondRequestSlowHeaderClosesConnection");

    require(second_response.empty(),
            "slow keep-alive second header should close the connection");
    require(g_request_count.load() == 1,
            "server should not dispatch slow keep-alive second header");

    g_stage = 13;
    server.stop();
}

void verify_keep_alive_second_request_body_timeout_closes_connection()
{
    g_request_count.store(0);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 1s;
    policy.timeouts.request_body_timeout = 200ms;
    policy.keep_alive.keep_alive_idle_timeout = 1s;

    const uint16_t port = pick_free_port();
    auto server = make_server(port, policy);

    g_stage = 14;
    server.start(make_router());

    g_stage = 15;
    int fd = connect_with_retry(port, 700ms);
    send_all(fd,
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
    std::string first_response = recv_once(fd);
    assert_contains(first_response, "HTTP/1.1 200 OK");
    assert_contains(first_response, "x-request-count: 1");

    send_all(fd,
            "POST /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 4\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
    std::string second_response = recv_once(fd);
    close_fd(fd, "verifyKeepAliveSecondRequestBodyTimeoutClosesConnection");

    require(second_response.empty(),
            "slow keep-alive second body should close the connection");
    require(g_request_count.load() == 1,
            "server should not dispatch keep-alive second request when body times out");

    g_stage = 16;
    server.stop();
}

void verify_response_write_timeout_interrupts_large_handler_send()
{
    g_slow_send_finished.store(false);
    g_slow_send_result.store(0);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 1s;
    policy.timeouts.response_write_timeout = 200ms;

    const uint16_t port = pick_free_port();
    auto server = make_server(port, policy);

    g_stage = 17;
    server.start(make_router());

    g_stage = 18;
    int fd = connect_with_retry(port, 300ms);
    set_socket_receive_buffer(fd, 1024);
    send_all(fd,
            "GET /slow HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");
    std::this_thread::sleep_for(800ms);
    close_fd(fd, "verifyResponseWriteTimeoutInterruptsLargeHandlerSend");

    require(g_slow_send_finished.load(),
            "large response send should complete promptly once response_write_timeout is enforced");
    require(g_slow_send_result.load() == -static_cast<int>(kSendTimeOut),
            "large response send should fail with kSendTimeOut under response_write_timeout backpressure");

    g_stage = 19;
    server.stop();
}

void verify_static_sendfile_write_timeout_closes_hung_connection()
{
    g_static_sendfile_finished.store(false);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 1s;
    policy.timeouts.response_write_timeout = 200ms;

    const std::string test_dir = "./tmp_t85_static_sendfile_" + std::to_string(::getpid());
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    create_large_file(test_dir + "/large.bin", 512ULL * 1024 * 1024);

    StaticFileSetting config;
    config.set_transfer_mode(FileTransferMode::SENDFILE);
    config.set_send_file_chunk_size(64 * 1024);

    HttpRouter router;
    router.mount("/static", test_dir, config);
    auto match = router.find_handler(HttpMethod::GET, "/static/large.bin");
    require(match.handler != nullptr,
            "mounted static sendfile route should resolve before wrapping the handler");
    HttpRouteHandler static_handler = *match.handler;
    router.add_handler<HttpMethod::GET>("/static/large.bin",
        [static_handler](HttpConn& conn, HttpRequest req) -> Task<void> {
            co_await static_handler(conn, std::move(req));
            g_static_sendfile_finished.store(true);
            co_return;
        });

    const uint16_t port = pick_free_port();
    auto server = make_server(port, policy);
    require(server.add_accept_plugin(std::make_unique<SetSendBufferPlugin>(1024)),
            "failed to install send buffer accept plugin for static sendfile timeout test");

    g_stage = 20;
    server.start(std::move(router));

    g_stage = 21;
    int fd = connect_with_retry(port, 1500ms);
    set_socket_receive_buffer(fd, 1024);
    send_all(fd,
            "GET /static/large.bin HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    bool finished_before_client_close = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_static_sendfile_finished.load()) {
            finished_before_client_close = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }

    close_fd(fd, "verifyStaticSendfileWriteTimeoutClosesHungConnection");
    server.stop();

    fs::remove_all(test_dir);
    require(finished_before_client_close,
            "static sendfile handler should finish promptly once response_write_timeout is enforced");
}

} // namespace

int main()
{
    ::signal(SIGALRM, alarm_handler);
    ::alarm(20);

    verify_initial_request_timeout_returns408();
    verify_keep_alive_idle_timeout_closes_before_next_request();
    verify_initial_request_body_timeout_returns408();
    verify_keep_alive_second_request_slow_header_closes_connection();
    verify_keep_alive_second_request_body_timeout_closes_connection();
    verify_response_write_timeout_interrupts_large_handler_send();
    verify_static_sendfile_write_timeout_closes_hung_connection();

    ::alarm(0);
    return 0;
}
