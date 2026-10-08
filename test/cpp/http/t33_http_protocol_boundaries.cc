#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <galay/cpp/galay-http/kernel/http_conn.h>
#include <galay/cpp/galay-http/kernel/http_reader.h>
#include <galay/cpp/galay-http/kernel/http_session.h>
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-http/server/http_router.h>
#include <galay/cpp/galay-http/server/http_server.h>

using galay::utils::RingBuffer;
using namespace galay::http;

namespace {

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "[T33] " << message << "\n";
    std::abort();
}

void check(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

std::vector<iovec> one_iovec(std::string& text)
{
    return {
        iovec{
            .iov_base = text.data(),
            .iov_len = text.size(),
        },
    };
}

void expect_request_error(std::string raw, HttpErrorCode expected, const std::string& label)
{
    HttpRequest request;
    auto iovecs = one_iovec(raw);
    const auto [err, consumed] = request.from_io_vec(iovecs);
    check(err == expected,
          label + " expected error " + std::to_string(expected) +
              " but got " + std::to_string(err) +
              " consumed=" + std::to_string(consumed));
}

void test_incomplete_header_preserves_incomplete()
{
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "X-Long: ";
    HttpRequest request;
    auto iovecs = one_iovec(raw);
    const auto [err, consumed] = request.from_io_vec(iovecs);

    check(err == kNoError, "partial direct parser keeps legacy kNoError contract");
    check(consumed == static_cast<ssize_t>(raw.size()), "incomplete header should report consumed bytes");
    check(!request.is_complete(), "incomplete header must not complete the request");
}

void test_reader_rejects_oversized_incomplete_header()
{
    RingBuffer ring_buffer{256};
    HttpReaderSetting setting;
    setting.set_max_header_size(48);
    HttpRequest request;
    galay::http::detail::HttpRequestReadState state(ring_buffer, setting, request);

    const std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "X-Oversized: " + std::string(80, 'a');

    check(ring_buffer.try_write_batch(raw.data(), raw.size()) == raw.size(),
          "failed to seed ring buffer");
    state.on_bytes_received(raw.size());

    check(state.parse_from_ring_buffer(), "oversized incomplete header should complete with an error");
    const auto result = state.take_result();
    check(!result.has_value(), "oversized incomplete header should not parse successfully");
    check(result.error().code() == kHeaderTooLarge,
          "oversized incomplete header should return kHeaderTooLarge");
}

void test_framing_headers_are_rejected()
{
    expect_request_error(
        "POST /upload HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "0\r\n\r\n",
        kBadRequest,
        "TE+CL request");

    expect_request_error(
        "POST /upload HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: gzip\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n",
        kBadRequest,
        "duplicate Transfer-Encoding request");

    expect_request_error(
        "POST /upload HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 0\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        kBadRequest,
        "duplicate Content-Length request");
}

void test_truncated_uri_escapes_are_rejected()
{
    expect_request_error("GET /bad% HTTP/1.1\r\nHost: example.com\r\n\r\n",
                       kUriEncodeError,
                       "trailing percent URI");
    expect_request_error("GET /bad%A HTTP/1.1\r\nHost: example.com\r\n\r\n",
                       kUriEncodeError,
                       "truncated hex URI");
    expect_request_error("GET /bad%u1 HTTP/1.1\r\nHost: example.com\r\n\r\n",
                       kUriEncodeError,
                       "truncated unicode URI");
}

void test_range_amplification_caps()
{
    std::string too_many = "bytes=";
    for (size_t i = 0; i < 20; ++i) {
        if (i != 0) {
            too_many += ",";
        }
        too_many += std::to_string(i * 2) + "-" + std::to_string(i * 2);
    }

    auto too_many_result = HttpRangeParser::parse(too_many, 1024);
    check(!too_many_result.is_valid(), "Range parser should reject too many ranges");

    auto merged = HttpRangeParser::parse("bytes=0-9,5-14,15-19", 100);
    check(merged.is_valid(), "overlapping ranges should still be valid after merge");
    check(merged.ranges.size() == 1, "overlapping/adjacent ranges should merge into one range");
    check(merged.ranges[0].start == 0 && merged.ranges[0].end == 19,
          "merged range should cover bytes 0-19");

    std::string too_large = "bytes=";
    constexpr uint64_t one_mib = 1024 * 1024;
    for (uint64_t i = 0; i < 9; ++i) {
        if (i != 0) {
            too_large += ",";
        }
        const uint64_t start = i * one_mib;
        too_large += std::to_string(start) + "-" + std::to_string(start + one_mib - 1);
    }
    auto too_large_result = HttpRangeParser::parse(too_large, 16 * one_mib);
    check(!too_large_result.is_valid(), "multipart Range parser should cap aggregate output bytes");
}

void test_session_rejects_oversized_response_body()
{
    AsyncTcpSocket socket;
    HttpReaderSetting setting;
    setting.set_max_body_size(4);
    HttpSession session(socket, 1024, setting);
    galay::http::detail::HttpSessionState<galay::async::AsyncTcpSocket> state(
        session, std::string("GET / HTTP/1.1\r\n\r\n"));

    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "12345";

    check(session.get_ring_buffer().try_write_batch(raw.data(), raw.size()) ==
              raw.size(),
          "failed to seed oversized response body");
    state.on_bytes_received(raw.size());

    check(state.parse_from_ring_buffer(), "oversized response body should complete with an error");
    const auto result = state.take_result();
    check(!result.has_value(), "oversized response body should not parse successfully");
    check(result.error().code() == kRequestEntityTooLarge,
          "oversized response body should return kRequestEntityTooLarge");
}

uint16_t reserve_free_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("socket() failed while reserving port");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        fail("bind() failed while reserving port");
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        fail("getsockname() failed while reserving port");
    }

    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

int connect_with_retry(uint16_t port)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            fail("socket() failed while connecting to test server");
        }

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }

        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    fail("could not connect to static HEAD test server");
}

std::string request_head(uint16_t port)
{
    const int fd = connect_with_retry(port);
    const std::string request =
        "HEAD /static/head.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n"
        "\r\n";
    check(::send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()),
          "failed to send HEAD request");

    std::string response;
    char buffer[1024];
    while (true) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            response.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        ::close(fd);
        fail("recv() failed while reading HEAD response");
    }
    ::close(fd);
    return response;
}

void test_static_head_does_not_send_body()
{
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() /
        ("galay-http-head-" + std::to_string(static_cast<long long>(::getpid())));
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream file(dir / "head.txt", std::ios::binary);
        file << "STATIC-BODY";
    }

    StaticFileSetting setting;
    setting.set_transfer_mode(FileTransferMode::MEMORY);

    HttpRouter router;
    router.mount("/static", dir.string(), setting);

    const uint16_t port = reserve_free_port();
    HttpServer server(HttpServerBuilder<>()
        .host("127.0.0.1")
        .port(port)
        .io_scheduler_count(1)
        .parallel_scheduler_count(1)
        .build_config());
    server.start(std::move(router));

    const std::string response = request_head(port);
    server.stop();
    fs::remove_all(dir);

    const auto split = response.find("\r\n\r\n");
    check(split != std::string::npos, "HEAD response should contain header terminator");
    const std::string body = response.substr(split + 4);
    check(response.find("HTTP/1.1 200") != std::string::npos, "HEAD response should be 200");
    check(body.empty(), "static HEAD response must not send a body");
}

void test_mount_hardly_registers_head()
{
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() /
        ("galay-http-hard-head-" + std::to_string(static_cast<long long>(::getpid())));
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream file(dir / "hard.txt", std::ios::binary);
        file << "hard";
    }

    HttpRouter router;
    router.mount_hardly("/hard", dir.string());
    fs::remove_all(dir);

    check(router.find_handler(HttpMethod::GET, "/hard/hard.txt").handler != nullptr,
          "mountHardly should register GET");
    check(router.find_handler(HttpMethod::HEAD, "/hard/hard.txt").handler != nullptr,
          "mountHardly should register HEAD");
}

} // namespace

int main()
{
    test_incomplete_header_preserves_incomplete();
    test_reader_rejects_oversized_incomplete_header();
    test_framing_headers_are_rejected();
    test_truncated_uri_escapes_are_rejected();
    test_range_amplification_caps();
    test_session_rejects_oversized_response_body();
    test_mount_hardly_registers_head();
    test_static_head_does_not_send_body();

    std::cout << "T33-HttpProtocolBoundaries PASS\n";
    return 0;
}
