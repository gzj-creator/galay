#include <galay/cpp/galay-http/kernel/http_writer.h>
#include <galay/cpp/galay-http/server/http_policy.h>
#include <galay/cpp/galay-http/server/http_router.h>
#include <galay/cpp/galay-http/server/http_server.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <string>
#include <utility>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace galay::http;
using namespace std::chrono_literals;

namespace {

volatile sig_atomic_t g_stage = 0;

void alarm_handler(int)
{
    const int stage = g_stage;
    std::cerr << "[T84] timeout at stage " << stage << "\n";
    ::_exit(2);
}

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "[T84] " << message << "\n";
    std::abort();
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

void require_code(HttpErrorCode actual, HttpErrorCode expected, const std::string& message)
{
    if (actual == expected) {
        return;
    }

    fail(message + ", actual=" + std::to_string(static_cast<int>(actual)) +
         " (" + HttpError(actual).message() + ")");
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
    int pton_ok = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (pton_ok != 1) {
        close_fd(fd, "pickFreePort inet_pton");
        fail("inet_pton failed while picking a free port");
    }

    int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        close_fd(fd, "pickFreePort bind");
        fail("bind failed while picking a free port");
    }

    socklen_t len = sizeof(addr);
    rc = ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    if (rc != 0) {
        close_fd(fd, "pickFreePort getsockname");
        fail("getsockname failed while picking a free port");
    }

    uint16_t port = ntohs(addr.sin_port);
    close_fd(fd, "pickFreePort success");
    return port;
}

int connect_with_retry(uint16_t port)
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
                close_fd(fd, "connectWithRetry setsockopt");
                fail("setsockopt timeout failed while connecting client");
            }
            return fd;
        }

        int err = errno;
        close_fd(fd, "connectWithRetry failed attempt");
        if (err == ECONNREFUSED || err == ETIMEDOUT || err == EHOSTUNREACH || err == ENETUNREACH) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        fail("connect failed, errno=" + std::to_string(err));
    }

    fail("connect retry exhausted");
}

void send_all(int fd, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            fail("send failed, errno=" + std::to_string(errno));
        }
        sent += static_cast<size_t>(n);
    }
}

std::string recv_until_closed(int fd)
{
    std::string response;
    char buffer[4096];
    while (true) {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            fail("recv failed, errno=" + std::to_string(errno));
        }
        response.append(buffer, static_cast<size_t>(n));
    }
    return response;
}

std::string send_raw_http(uint16_t port, const std::string& request)
{
    int fd = connect_with_retry(port);
    send_all(fd, request);
    std::string response = recv_until_closed(fd);
    close_fd(fd, "sendRawHttp");
    return response;
}

void assert_contains(const std::string& text, const std::string& expected)
{
    if (text.find(expected) == std::string::npos) {
        std::cerr << "[T84] expected substring not found: " << expected << "\n";
        std::cerr << "[T84] actual response prefix:\n" << text.substr(0, 500) << "\n";
        std::abort();
    }
}

HttpErrorCode parse_request_with_setting(const std::string& raw, const HttpReaderSetting& setting) {
    galay::utils::RingBuffer ring_buffer{HttpConn::kDefaultRingBufferSize};
    HttpRequest request;
    galay::http::detail::HttpRequestReadState state(ring_buffer, setting, request);

    size_t offset = 0;
    while (true) {
        if (state.parse_from_ring_buffer()) {
            auto result = state.take_result();
            return result ? kNoError : result.error().code();
        }

        if (offset >= raw.size()) {
            return kIncomplete;
        }

        if (!state.prepare_recv_window()) {
            auto result = state.take_result();
            return result ? kNoError : result.error().code();
        }

        const struct iovec* windows = state.recv_iovecs_data();
        const size_t window_count = state.recv_iovecs_count();
        size_t copied = 0;
        for (size_t i = 0; i < window_count && offset + copied < raw.size(); ++i) {
            const size_t bytes = std::min(windows[i].iov_len, raw.size() - offset - copied);
            std::memcpy(windows[i].iov_base, raw.data() + offset + copied, bytes);
            copied += bytes;
        }
        if (copied == 0) {
            fail("reader produced no writable window");
        }

        state.on_bytes_received(copied);
        offset += copied;
    }
}

void verify_policy_surface_compiles() {
    HttpServerPolicy policy;

    require(policy.request_limits.max_header_size == DEFAULT_HTTP_MAX_HEADER_SIZE,
            "default header size mismatch");
    require(policy.request_limits.max_body_size == DEFAULT_HTTP_MAX_BODY_SIZE,
            "default body size mismatch");
    require(policy.timeouts.request_header_timeout.count() == DEFAULT_HTTP_RECV_TIME_MS,
            "default header timeout mismatch");
    require(policy.timeouts.response_write_timeout.count() == DEFAULT_HTTP_SEND_TIME_MS,
            "default response timeout mismatch");
    require(policy.keep_alive.enabled, "default keep-alive should be enabled");

    policy.request_limits.max_header_count = 32;
    policy.request_limits.max_header_line_size = 4096;
    policy.request_limits.max_uri_size = 2048;
    policy.keep_alive.max_requests_per_connection = 100;
    policy.keep_alive.keep_alive_idle_timeout = 1500ms;
    policy.timeouts.request_body_timeout = 2s;
    policy.proxy.max_idle_connections_per_upstream = 4;

    HttpServerBuilder builder;
    auto config = builder.policy(policy).build_config();
    require(config.policy.request_limits.max_header_count == 32,
            "builder policy did not preserve header count");
    require(config.policy.keep_alive.max_requests_per_connection == 100,
            "builder policy did not preserve keep-alive max requests");

    HttpRouter router;
    router.set_default_policy(config.policy);
    require(router.default_policy().proxy.max_idle_connections_per_upstream == 4,
            "router policy did not preserve proxy idle limit");

    auto server = builder.build();
    require(!server.is_running(),
            "builder build should create a stopped server instance");
}

void verify_writer_timeout_maps_to_send_timeout()
{
    require(galay::kernel::IOError::contains(
                galay::kernel::IOError(galay::kernel::kTimeout, 0).code(),
                galay::kernel::kTimeout),
            "IOError(kTimeout, 0) should be recognized as timeout");
    require_code(galay::http::detail::make_send_http_error(
                    galay::kernel::IOError(galay::kernel::kTimeout, 0))
                    .code(),
                kSendTimeOut,
                "writer timeout helper should map to kSendTimeOut");

    AsyncTcpSocket socket;
    HttpWriterSetting setting;
    HttpWriterImpl<AsyncTcpSocket> writer(setting, socket);
    [[maybe_unused]] auto send_operation = writer.send("x", 1);
    require(writer.get_remaining_bytes() == 1,
            "writer send should stage one byte before advancing write machine");

    galay::http::detail::HttpTcpWriteMachine<galay::async::AsyncTcpSocket, false> machine(&writer);
    machine.on_write(std::unexpected(galay::kernel::IOError(galay::kernel::kTimeout, 0)));
    auto action = machine.advance();

    require(action.signal == galay::kernel::MachineSignal::kComplete,
            "writer timeout should complete with an HttpError result");
    require(action.result.has_value() && !action.result->has_value(),
            "writer timeout should return an expected error result");
    require_code(action.result->error().code(), kSendTimeOut,
                "writer timeout should map to kSendTimeOut");
}

void verify_request_limits() {
    HttpReaderSetting setting;
    setting.set_max_header_count(2);
    setting.set_max_header_line_size(32);
    setting.set_max_uri_size(8);
    setting.set_max_body_size(4);

    const std::string too_many_headers =
        "GET /ok HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-One: 1\r\n"
        "X-Two: 2\r\n"
        "\r\n";
    require_code(parse_request_with_setting(too_many_headers, setting), kHeaderTooLarge,
                "too many headers should return kHeaderTooLarge");

    const std::string long_line =
        "GET /ok HTTP/1.1\r\n"
        "X-Long: 123456789012345678901234567890\r\n"
        "\r\n";
    require_code(parse_request_with_setting(long_line, setting), kHeaderTooLarge,
                "oversized header line should return kHeaderTooLarge");

    const std::string long_uri =
        "GET /0123456789 HTTP/1.1\r\n"
        "\r\n";
    require_code(parse_request_with_setting(long_uri, setting), kUriTooLong,
                "oversized URI should return kUriTooLong");

    const std::string invalid_content_length =
        "POST /ok HTTP/1.1\r\n"
        "Content-Length: nope\r\n"
        "\r\n";
    require_code(parse_request_with_setting(invalid_content_length, setting), kBadRequest,
                "invalid Content-Length should return kBadRequest");

    const std::string conflicting_content_length =
        "POST /ok HTTP/1.1\r\n"
        "Content-Length: 3\r\n"
        "Content-Length: 4\r\n"
        "\r\n";
    require_code(parse_request_with_setting(conflicting_content_length, setting), kBadRequest,
                "conflicting Content-Length should return kBadRequest");

    const std::string transfer_encoding_smuggling =
        "POST /ok HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 3\r\n"
        "\r\n"
        "0\r\n\r\n";
    require_code(parse_request_with_setting(transfer_encoding_smuggling, setting), kBadRequest,
                "chunked request with Content-Length should return kBadRequest");

    const std::string oversized_body =
        "POST /ok HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "12345";
    require_code(parse_request_with_setting(oversized_body, setting), kRequestEntityTooLarge,
                "oversized declared body should return kRequestEntityTooLarge");
}

Task<void> ok_handler(HttpConn& conn, HttpRequest)
{
    auto response = Http1_1ResponseBuilder::ok()
        .header("Connection", "close")
        .text("ok")
        .build_move();
    auto writer = conn.get_writer();
    auto result = co_await writer.send_response(response);
    if (!result) {
        fail("ok handler send failed: " + result.error().message());
    }
    co_return;
}

void verify_route_mode_limit_responses()
{
    HttpServerPolicy policy;
    policy.request_limits.max_header_count = 3;
    policy.request_limits.max_header_line_size = 32;
    policy.request_limits.max_uri_size = 8;
    policy.request_limits.max_body_size = 4;
    uint16_t port = pick_free_port();
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .io_scheduler_count(1)
        .parallel_scheduler_count(1)
        .policy(policy)
        .build());

    HttpRouter router;
    router.add_handler<HttpMethod::GET, HttpMethod::POST>("/ok", ok_handler);
    router.add_handler<HttpMethod::GET>("/0123456789", ok_handler);

    g_stage = 2;
    server.start(std::move(router));

    g_stage = 3;
    assert_contains(send_raw_http(port,
        "GET /ok HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-One: 1\r\n"
        "X-Two: 2\r\n"
        "Connection: close\r\n"
        "\r\n"), "HTTP/1.1 431 Request Header Fields Too Large");

    g_stage = 4;
    assert_contains(send_raw_http(port,
        "GET /ok HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Long: 123456789012345678901234567890\r\n"
        "Connection: close\r\n"
        "\r\n"), "HTTP/1.1 431 Request Header Fields Too Large");

    g_stage = 5;
    assert_contains(send_raw_http(port,
        "GET /0123456789 HTTP/1.1\r\n"
        "Connection: close\r\n"
        "\r\n"), "HTTP/1.1 414 URI Too Long");

    g_stage = 6;
    assert_contains(send_raw_http(port,
        "POST /ok HTTP/1.1\r\n"
        "Content-Length: nope\r\n"
        "Connection: close\r\n"
        "\r\n"), "HTTP/1.1 400 Bad Request");

    g_stage = 7;
    assert_contains(send_raw_http(port,
        "POST /ok HTTP/1.1\r\n"
        "Content-Length: 3\r\n"
        "Content-Length: 4\r\n"
        "Connection: close\r\n"
        "\r\n"), "HTTP/1.1 400 Bad Request");

    g_stage = 8;
    assert_contains(send_raw_http(port,
        "POST /ok HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 3\r\n"
        "Connection: close\r\n"
        "\r\n"
        "0\r\n\r\n"), "HTTP/1.1 400 Bad Request");

    g_stage = 9;
    assert_contains(send_raw_http(port,
        "POST /ok HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "12345"), "HTTP/1.1 413 Payload Too Large");

    g_stage = 10;
    server.stop();
}

} // namespace

int main() {
    ::signal(SIGALRM, alarm_handler);
    ::alarm(20);

    g_stage = 1;
    verify_writer_timeout_maps_to_send_timeout();
    verify_policy_surface_compiles();
    verify_request_limits();
    verify_route_mode_limit_responses();

    ::alarm(0);
    return 0;
}
