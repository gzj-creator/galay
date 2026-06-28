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

class SetSendBufferPlugin final : public plugin::AcceptPlugin<TcpSocket> {
public:
    explicit SetSendBufferPlugin(int size)
        : m_size(size) {}

    Task<bool> handle(Runtime&, TcpSocket& socket, const Host&) override
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

void alarmHandler(int)
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

void closeFd(int fd, const char* context)
{
    if (::close(fd) != 0) {
        fail(std::string(context) + ": close failed, errno=" + std::to_string(errno));
    }
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
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        closeFd(fd, "pickFreePort inet_pton");
        fail("inet_pton failed while picking a free port");
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeFd(fd, "pickFreePort bind");
        fail("bind failed while picking a free port");
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        closeFd(fd, "pickFreePort getsockname");
        fail("getsockname failed while picking a free port");
    }

    const uint16_t port = ntohs(addr.sin_port);
    closeFd(fd, "pickFreePort success");
    return port;
}

int connectWithRetry(uint16_t port, std::chrono::milliseconds recv_timeout)
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
                closeFd(fd, "connectWithRetry setsockopt");
                fail("setsockopt timeout failed while connecting client");
            }
            return fd;
        }

        const int err = errno;
        closeFd(fd, "connectWithRetry failed attempt");
        if (err == ECONNREFUSED || err == ETIMEDOUT || err == EHOSTUNREACH || err == ENETUNREACH) {
            std::this_thread::sleep_for(20ms);
            continue;
        }

        fail("connect failed, errno=" + std::to_string(err));
    }

    fail("connect retry exhausted");
}

void sendAll(int fd, std::string_view data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            fail("send failed, errno=" + std::to_string(errno));
        }
        sent += static_cast<size_t>(n);
    }
}

std::string recvOnce(int fd)
{
    char buffer[4096];
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n < 0) {
        fail("recv failed at stage " + std::to_string(g_stage) + ", errno=" + std::to_string(errno));
    }
    if (n == 0) {
        return {};
    }
    return std::string(buffer, static_cast<size_t>(n));
}

void setSocketReceiveBuffer(int fd, int size)
{
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0) {
        fail("setsockopt SO_RCVBUF failed, errno=" + std::to_string(errno));
    }
}

void assertContains(const std::string& text, const std::string& expected)
{
    if (text.find(expected) == std::string::npos) {
        std::cerr << "[T85] expected substring not found: " << expected << "\n";
        std::cerr << "[T85] actual payload:\n" << text << "\n";
        std::abort();
    }
}

void createLargeFile(const std::string& path, size_t size)
{
    int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fail("open sparse file for static sendfile timeout test failed, errno=" + std::to_string(errno));
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        closeFd(fd, "createLargeFile ftruncate");
        fail("ftruncate sparse file for static sendfile timeout test failed, errno=" + std::to_string(errno));
    }
    closeFd(fd, "createLargeFile success");
}

Task<void> okHandler(HttpConn& conn, HttpRequest request)
{
    const int request_no = g_request_count.fetch_add(1) + 1;
    auto response = Http1_1ResponseBuilder::ok()
        .header("Content-Type", "text/plain")
        .header("X-Request-Count", std::to_string(request_no))
        .header("Connection", request.header().isKeepAlive() ? "keep-alive" : "close")
        .text("ok")
        .buildMove();
    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);
    if (!result) {
        fail("ok handler send failed: " + result.error().message());
    }
    co_return;
}

Task<void> slowResponseHandler(HttpConn& conn, HttpRequest)
{
    constexpr size_t kSlowResponseBodySize = 16 * 1024 * 1024;
    int send_buffer_size = 1024;
    if (::setsockopt(conn.getSocket().handle().fd,
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
        .buildMove();
    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);
    g_slow_send_result.store(result ? 1 : -static_cast<int>(result.error().code()));
    g_slow_send_finished.store(true);
    co_return;
}

HttpServer makeServer(uint16_t port, HttpServerPolicy policy)
{
    return HttpServer(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .policy(std::move(policy))
        .build());
}

HttpRouter makeRouter()
{
    HttpRouter router;
    router.addHandler<HttpMethod::GET, HttpMethod::POST>("/ok", okHandler);
    router.addHandler<HttpMethod::GET>("/slow", slowResponseHandler);
    return router;
}

void verifyInitialRequestTimeoutReturns408()
{
    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 200ms;
    policy.keep_alive.keep_alive_idle_timeout = 200ms;

    const uint16_t port = pickFreePort();
    auto server = makeServer(port, policy);

    g_stage = 1;
    server.start(makeRouter());

    g_stage = 2;
    int fd = connectWithRetry(port, 1500ms);
    std::string response = recvOnce(fd);
    closeFd(fd, "verifyInitialRequestTimeoutReturns408");

    assertContains(response, "HTTP/1.1 408 Request Timeout");
    assertContains(response, "connection: close");

    g_stage = 3;
    server.stop();
}

void verifyKeepAliveIdleTimeoutClosesBeforeNextRequest()
{
    g_request_count.store(0);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 300ms;
    policy.keep_alive.keep_alive_idle_timeout = 200ms;

    const uint16_t port = pickFreePort();
    auto server = makeServer(port, policy);

    g_stage = 4;
    server.start(makeRouter());

    g_stage = 5;
    int fd = connectWithRetry(port, 1500ms);
    sendAll(fd,
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
    std::string first_response = recvOnce(fd);
    assertContains(first_response, "HTTP/1.1 200 OK");
    assertContains(first_response, "x-request-count: 1");

    std::this_thread::sleep_for(350ms);

    g_stage = 6;
    sendAll(fd,
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");
    std::string second_response = recvOnce(fd);
    closeFd(fd, "verifyKeepAliveIdleTimeoutClosesBeforeNextRequest");

    require(second_response.empty(),
            "idle keep-alive connection should be closed before second request");
    require(g_request_count.load() == 1,
            "server should not dispatch second request after idle timeout");

    g_stage = 7;
    server.stop();
}

void verifyInitialRequestBodyTimeoutReturns408()
{
    g_request_count.store(0);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 1s;
    policy.timeouts.request_body_timeout = 200ms;
    policy.keep_alive.keep_alive_idle_timeout = 1s;

    const uint16_t port = pickFreePort();
    auto server = makeServer(port, policy);

    g_stage = 8;
    server.start(makeRouter());

    g_stage = 9;
    int fd = connectWithRetry(port, 700ms);
    sendAll(fd,
            "POST /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 4\r\n"
            "Connection: close\r\n"
            "\r\n");
    std::string response = recvOnce(fd);
    closeFd(fd, "verifyInitialRequestBodyTimeoutReturns408");

    assertContains(response, "HTTP/1.1 408 Request Timeout");
    assertContains(response, "connection: close");
    require(g_request_count.load() == 0,
            "server should not dispatch request when body times out");

    g_stage = 10;
    server.stop();
}

void verifyKeepAliveSecondRequestSlowHeaderClosesConnection()
{
    g_request_count.store(0);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 1s;
    policy.keep_alive.keep_alive_idle_timeout = 200ms;

    const uint16_t port = pickFreePort();
    auto server = makeServer(port, policy);

    g_stage = 11;
    server.start(makeRouter());

    g_stage = 12;
    int fd = connectWithRetry(port, 700ms);
    sendAll(fd,
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
    std::string first_response = recvOnce(fd);
    assertContains(first_response, "HTTP/1.1 200 OK");
    assertContains(first_response, "x-request-count: 1");

    sendAll(fd,
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n");
    std::string second_response = recvOnce(fd);
    closeFd(fd, "verifyKeepAliveSecondRequestSlowHeaderClosesConnection");

    require(second_response.empty(),
            "slow keep-alive second header should close the connection");
    require(g_request_count.load() == 1,
            "server should not dispatch slow keep-alive second header");

    g_stage = 13;
    server.stop();
}

void verifyKeepAliveSecondRequestBodyTimeoutClosesConnection()
{
    g_request_count.store(0);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 1s;
    policy.timeouts.request_body_timeout = 200ms;
    policy.keep_alive.keep_alive_idle_timeout = 1s;

    const uint16_t port = pickFreePort();
    auto server = makeServer(port, policy);

    g_stage = 14;
    server.start(makeRouter());

    g_stage = 15;
    int fd = connectWithRetry(port, 700ms);
    sendAll(fd,
            "GET /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
    std::string first_response = recvOnce(fd);
    assertContains(first_response, "HTTP/1.1 200 OK");
    assertContains(first_response, "x-request-count: 1");

    sendAll(fd,
            "POST /ok HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 4\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
    std::string second_response = recvOnce(fd);
    closeFd(fd, "verifyKeepAliveSecondRequestBodyTimeoutClosesConnection");

    require(second_response.empty(),
            "slow keep-alive second body should close the connection");
    require(g_request_count.load() == 1,
            "server should not dispatch keep-alive second request when body times out");

    g_stage = 16;
    server.stop();
}

void verifyResponseWriteTimeoutInterruptsLargeHandlerSend()
{
    g_slow_send_finished.store(false);
    g_slow_send_result.store(0);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 1s;
    policy.timeouts.response_write_timeout = 200ms;

    const uint16_t port = pickFreePort();
    auto server = makeServer(port, policy);

    g_stage = 17;
    server.start(makeRouter());

    g_stage = 18;
    int fd = connectWithRetry(port, 300ms);
    setSocketReceiveBuffer(fd, 1024);
    sendAll(fd,
            "GET /slow HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");
    std::this_thread::sleep_for(800ms);
    closeFd(fd, "verifyResponseWriteTimeoutInterruptsLargeHandlerSend");

    require(g_slow_send_finished.load(),
            "large response send should complete promptly once response_write_timeout is enforced");
    require(g_slow_send_result.load() == -static_cast<int>(kSendTimeOut),
            "large response send should fail with kSendTimeOut under response_write_timeout backpressure");

    g_stage = 19;
    server.stop();
}

void verifyStaticSendfileWriteTimeoutClosesHungConnection()
{
    g_static_sendfile_finished.store(false);

    HttpServerPolicy policy;
    policy.timeouts.request_header_timeout = 1s;
    policy.timeouts.response_write_timeout = 200ms;

    const std::string test_dir = "./tmp_t85_static_sendfile_" + std::to_string(::getpid());
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    createLargeFile(test_dir + "/large.bin", 512ULL * 1024 * 1024);

    StaticFileSetting config;
    config.setTransferMode(FileTransferMode::SENDFILE);
    config.setSendFileChunkSize(64 * 1024);

    HttpRouter router;
    router.mount("/static", test_dir, config);
    auto match = router.findHandler(HttpMethod::GET, "/static/large.bin");
    require(match.handler != nullptr,
            "mounted static sendfile route should resolve before wrapping the handler");
    HttpRouteHandler static_handler = *match.handler;
    router.addHandler<HttpMethod::GET>("/static/large.bin",
        [static_handler](HttpConn& conn, HttpRequest req) -> Task<void> {
            co_await static_handler(conn, std::move(req));
            g_static_sendfile_finished.store(true);
            co_return;
        });

    const uint16_t port = pickFreePort();
    auto server = makeServer(port, policy);
    require(server.addAcceptPlugin(std::make_unique<SetSendBufferPlugin>(1024)),
            "failed to install send buffer accept plugin for static sendfile timeout test");

    g_stage = 20;
    server.start(std::move(router));

    g_stage = 21;
    int fd = connectWithRetry(port, 1500ms);
    setSocketReceiveBuffer(fd, 1024);
    sendAll(fd,
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

    closeFd(fd, "verifyStaticSendfileWriteTimeoutClosesHungConnection");
    server.stop();

    fs::remove_all(test_dir);
    require(finished_before_client_close,
            "static sendfile handler should finish promptly once response_write_timeout is enforced");
}

} // namespace

int main()
{
    ::signal(SIGALRM, alarmHandler);
    ::alarm(20);

    verifyInitialRequestTimeoutReturns408();
    verifyKeepAliveIdleTimeoutClosesBeforeNextRequest();
    verifyInitialRequestBodyTimeoutReturns408();
    verifyKeepAliveSecondRequestSlowHeaderClosesConnection();
    verifyKeepAliveSecondRequestBodyTimeoutClosesConnection();
    verifyResponseWriteTimeoutInterruptsLargeHandlerSend();
    verifyStaticSendfileWriteTimeoutClosesHungConnection();

    ::alarm(0);
    return 0;
}
