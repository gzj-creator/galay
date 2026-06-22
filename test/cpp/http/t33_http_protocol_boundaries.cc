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

std::vector<iovec> oneIovec(std::string& text)
{
    return {
        iovec{
            .iov_base = text.data(),
            .iov_len = text.size(),
        },
    };
}

void expectRequestError(std::string raw, HttpErrorCode expected, const std::string& label)
{
    HttpRequest request;
    auto iovecs = oneIovec(raw);
    const auto [err, consumed] = request.fromIOVec(iovecs);
    check(err == expected,
          label + " expected error " + std::to_string(expected) +
              " but got " + std::to_string(err) +
              " consumed=" + std::to_string(consumed));
}

void testIncompleteHeaderPreservesIncomplete()
{
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "X-Long: ";
    HttpRequest request;
    auto iovecs = oneIovec(raw);
    const auto [err, consumed] = request.fromIOVec(iovecs);

    check(err == kNoError, "partial direct parser keeps legacy kNoError contract");
    check(consumed == static_cast<ssize_t>(raw.size()), "incomplete header should report consumed bytes");
    check(!request.isComplete(), "incomplete header must not complete the request");
}

void testReaderRejectsOversizedIncompleteHeader()
{
    RingBuffer ring_buffer{256};
    HttpReaderSetting setting;
    setting.setMaxHeaderSize(48);
    HttpRequest request;
    galay::http::detail::HttpRequestReadState state(ring_buffer, setting, request);

    const std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "X-Oversized: " + std::string(80, 'a');

    check(ring_buffer.write(raw.data(), raw.size()) == raw.size(), "failed to seed ring buffer");
    state.onBytesReceived(raw.size());

    check(state.parseFromRingBuffer(), "oversized incomplete header should complete with an error");
    const auto result = state.takeResult();
    check(!result.has_value(), "oversized incomplete header should not parse successfully");
    check(result.error().code() == kHeaderTooLarge,
          "oversized incomplete header should return kHeaderTooLarge");
}

void testFramingHeadersAreRejected()
{
    expectRequestError(
        "POST /upload HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "0\r\n\r\n",
        kBadRequest,
        "TE+CL request");

    expectRequestError(
        "POST /upload HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: gzip\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n",
        kBadRequest,
        "duplicate Transfer-Encoding request");

    expectRequestError(
        "POST /upload HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 0\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        kBadRequest,
        "duplicate Content-Length request");
}

void testTruncatedUriEscapesAreRejected()
{
    expectRequestError("GET /bad% HTTP/1.1\r\nHost: example.com\r\n\r\n",
                       kUriEncodeError,
                       "trailing percent URI");
    expectRequestError("GET /bad%A HTTP/1.1\r\nHost: example.com\r\n\r\n",
                       kUriEncodeError,
                       "truncated hex URI");
    expectRequestError("GET /bad%u1 HTTP/1.1\r\nHost: example.com\r\n\r\n",
                       kUriEncodeError,
                       "truncated unicode URI");
}

void testRangeAmplificationCaps()
{
    std::string too_many = "bytes=";
    for (size_t i = 0; i < 20; ++i) {
        if (i != 0) {
            too_many += ",";
        }
        too_many += std::to_string(i * 2) + "-" + std::to_string(i * 2);
    }

    auto too_many_result = HttpRangeParser::parse(too_many, 1024);
    check(!too_many_result.isValid(), "Range parser should reject too many ranges");

    auto merged = HttpRangeParser::parse("bytes=0-9,5-14,15-19", 100);
    check(merged.isValid(), "overlapping ranges should still be valid after merge");
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
    check(!too_large_result.isValid(), "multipart Range parser should cap aggregate output bytes");
}

uint16_t reserveFreePort()
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

int connectWithRetry(uint16_t port)
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

std::string requestHead(uint16_t port)
{
    const int fd = connectWithRetry(port);
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

void testStaticHeadDoesNotSendBody()
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
    setting.setTransferMode(FileTransferMode::MEMORY);

    HttpRouter router;
    router.mount("/static", dir.string(), setting);

    const uint16_t port = reserveFreePort();
    auto server = HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build();
    server.start(std::move(router));

    const std::string response = requestHead(port);
    server.stop();
    fs::remove_all(dir);

    const auto split = response.find("\r\n\r\n");
    check(split != std::string::npos, "HEAD response should contain header terminator");
    const std::string body = response.substr(split + 4);
    check(response.find("HTTP/1.1 200") != std::string::npos, "HEAD response should be 200");
    check(body.empty(), "static HEAD response must not send a body");
}

void testMountHardlyRegistersHead()
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
    router.mountHardly("/hard", dir.string());
    fs::remove_all(dir);

    check(router.findHandler(HttpMethod::GET, "/hard/hard.txt").handler != nullptr,
          "mountHardly should register GET");
    check(router.findHandler(HttpMethod::HEAD, "/hard/hard.txt").handler != nullptr,
          "mountHardly should register HEAD");
}

} // namespace

int main()
{
    testIncompleteHeaderPreservesIncomplete();
    testReaderRejectsOversizedIncompleteHeader();
    testFramingHeadersAreRejected();
    testTruncatedUriEscapesAreRejected();
    testRangeAmplificationCaps();
    testMountHardlyRegistersHead();
    testStaticHeadDoesNotSendBody();

    std::cout << "T33-HttpProtocolBoundaries PASS\n";
    return 0;
}
