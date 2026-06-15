/**
 * @file t79_largeput.cc
 * @brief Regression test for large HTTP/1.1 PUT request bodies.
 */

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <type_traits>
#include <unistd.h>

#include "galay-http/kernel/http_conn.h"
#include "galay-http/server/http_server.h"
#include "galay-http2/protoc/http2_error.h"
#include "galay-ws/protoc/ws_error.h"
#include "galay-http/builder/http_builder.h"

using namespace galay::http;
using namespace galay::kernel;
using ::galay::utils::RingBuffer;

static_assert(std::is_constructible_v<HttpError, IOError>);
static_assert(std::is_constructible_v<galay::http2::Http2Error, IOError>);
static_assert(std::is_constructible_v<galay::websocket::WsError, IOError>);
static_assert(requires(HttpReader& reader, HttpRequest& request) {
    reader.getRequest(request).timeout(std::chrono::milliseconds(1));
});
static_assert(requires(HttpReader& reader, HttpResponse& response) {
    reader.getResponse(response).timeout(std::chrono::milliseconds(1));
});
static_assert(requires(HttpReader& reader, std::string& chunk_data) {
    reader.getChunk(chunk_data).timeout(std::chrono::milliseconds(1));
});

namespace {

constexpr size_t kBodySize = 8 * 1024 * 1024;
constexpr size_t kChunkBodySize = 128 * 1024;
volatile sig_atomic_t g_stage = 0;

void alarmHandler(int)
{
    const int stage = g_stage;
    const char* message = nullptr;
    switch (stage) {
    case 1:
        message = "[T79] timeout while counting receive windows\n";
        break;
    case 2:
        message = "[T79] timeout while starting server\n";
        break;
    case 3:
        message = "[T79] timeout while connecting client\n";
        break;
    case 4:
        message = "[T79] timeout while sending request\n";
        break;
    case 5:
        message = "[T79] timeout while receiving response\n";
        break;
    case 6:
        message = "[T79] timeout while stopping server\n";
        break;
    default:
        message = "[T79] timeout before test stage was set\n";
        break;
    }
    (void)::write(STDERR_FILENO, message, std::strlen(message));
    ::_exit(2);
}

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "[T79] " << message << "\n";
    std::abort();
}

std::string makeLargePutRequest()
{
    std::string body(kBodySize, '\0');
    for (size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<char>(i % 251);
    }

    std::string request;
    request.reserve(256 + body.size());
    request += "PUT /upload HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "Content-Type: application/octet-stream\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n";
    request += "\r\n";
    request += body;
    return request;
}

std::string makeLargeChunkStream()
{
    std::string body(kBodySize, '\0');
    for (size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<char>((i * 17) % 251);
    }
    return Chunk::toChunk(body, false) + Chunk::toChunk(std::string{}, true);
}

size_t countReadWindowsForDefaultHttpConn(std::string_view raw_request)
{
    RingBuffer ring_buffer{HttpConn::kDefaultRingBufferSize};
    HttpReaderSetting setting;
    setting.setMaxBodySize(kBodySize + 1024);
    HttpRequest request;
    galay::http::detail::HttpRequestReadState state(ring_buffer, setting, request);

    size_t offset = 0;
    size_t read_windows = 0;
    while (true) {
        if (state.parseFromRingBuffer()) {
            auto result = state.takeResult();
            if (!result.has_value()) {
                fail("request parse returned error: " + result.error().message());
            }
            if (!request.isComplete()) {
                fail("request parse completed without a complete request");
            }
            if (request.bodyStr().size() != kBodySize) {
                fail("parsed body size mismatch: " + std::to_string(request.bodyStr().size()));
            }
            if (offset != raw_request.size()) {
                fail("parser completed before all peer bytes were supplied");
            }
            return read_windows;
        }

        if (offset >= raw_request.size()) {
            fail("parser still needs bytes after the full request was supplied");
        }

        if (!state.prepareRecvWindow()) {
            fail("default HttpConn ring buffer cannot provide a receive window");
        }
        const struct iovec* windows = state.recvIovecsData();
        const size_t window_count = state.recvIovecsCount();
        if (windows == nullptr || window_count == 0) {
            fail("default HttpConn produced an empty receive window");
        }

        size_t copied = 0;
        for (size_t i = 0; i < window_count && offset + copied < raw_request.size(); ++i) {
            const size_t bytes = std::min(
                windows[i].iov_len,
                raw_request.size() - offset - copied);
            std::memcpy(windows[i].iov_base, raw_request.data() + offset + copied, bytes);
            copied += bytes;
        }
        if (copied == 0) {
            fail("receive window could not accept more request bytes");
        }

        state.onBytesReceived(copied);
        offset += copied;
        ++read_windows;
        if (read_windows >= 1024) {
            fail("request parse did not converge");
        }
    }
}

void verifyHugeContentLengthReturnsEntityTooLarge()
{
    RingBuffer ring_buffer{HttpConn::kDefaultRingBufferSize};
    HttpReaderSetting setting;
    HttpRequest request;
    galay::http::detail::HttpRequestReadState state(ring_buffer, setting, request);

    const std::string header =
        "PUT /upload HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: " + std::to_string(std::numeric_limits<size_t>::max()) + "\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (!state.prepareRecvWindow()) {
        fail("default HttpConn ring buffer cannot accept huge Content-Length header");
    }

    const struct iovec* windows = state.recvIovecsData();
    const size_t window_count = state.recvIovecsCount();
    size_t copied = 0;
    for (size_t i = 0; i < window_count && copied < header.size(); ++i) {
        const size_t bytes = std::min(windows[i].iov_len, header.size() - copied);
        std::memcpy(windows[i].iov_base, header.data() + copied, bytes);
        copied += bytes;
    }
    if (copied != header.size()) {
        fail("default HttpConn ring buffer could not fit huge Content-Length header");
    }
    state.onBytesReceived(copied);

    try {
        if (!state.parseFromRingBuffer()) {
            fail("huge Content-Length header did not complete with an error");
        }

        auto result = state.takeResult();
        if (result) {
            fail("huge Content-Length header completed successfully");
        }
        if (result.error().code() != kRequestEntityTooLarge) {
            fail("huge Content-Length returned wrong error: " + result.error().message());
        }
    } catch (const std::exception& ex) {
        fail("huge Content-Length header caused parser exception: " + std::string(ex.what()));
    } catch (...) {
        fail("huge Content-Length header caused unknown parser exception");
    }
}

void verifyChunkReaderReturnsEntityTooLarge()
{
    RingBuffer ring_buffer{HttpConn::kDefaultRingBufferSize};
    HttpReaderSetting setting;
    setting.setMaxBodySize(4);
    std::string chunk_data;
    galay::http::detail::HttpChunkReadState state(ring_buffer, setting, chunk_data);

    const std::string chunk = "5\r\nHello\r\n";
    if (!state.prepareRecvWindow()) {
        fail("chunk reader ring buffer cannot accept chunk data");
    }

    const struct iovec* windows = state.recvIovecsData();
    const size_t window_count = state.recvIovecsCount();
    size_t copied = 0;
    for (size_t i = 0; i < window_count && copied < chunk.size(); ++i) {
        const size_t bytes = std::min(windows[i].iov_len, chunk.size() - copied);
        std::memcpy(windows[i].iov_base, chunk.data() + copied, bytes);
        copied += bytes;
    }
    if (copied != chunk.size()) {
        fail("chunk reader ring buffer could not fit chunk data");
    }
    state.onBytesReceived(copied);

    if (!state.parseFromRingBuffer()) {
        fail("oversized chunk did not complete with an error");
    }

    auto result = state.takeResult();
    if (result) {
        fail("oversized chunk completed successfully");
    }
    if (result.error().code() != kRequestEntityTooLarge) {
        fail("oversized chunk returned wrong error: " + result.error().message());
    }
}

void verifyChunkReaderRejectsOversizeFromSizeLine()
{
    RingBuffer ring_buffer{4096};
    HttpReaderSetting setting;
    setting.setMaxBodySize(1024);
    std::string chunk_data;
    galay::http::detail::HttpChunkReadState state(ring_buffer, setting, chunk_data);

    const std::string chunk_header = "800\r\n";
    if (!state.prepareRecvWindow()) {
        fail("chunk reader ring buffer cannot accept oversized chunk header");
    }

    const struct iovec* windows = state.recvIovecsData();
    const size_t window_count = state.recvIovecsCount();
    size_t copied = 0;
    for (size_t i = 0; i < window_count && copied < chunk_header.size(); ++i) {
        const size_t bytes = std::min(windows[i].iov_len, chunk_header.size() - copied);
        std::memcpy(windows[i].iov_base, chunk_header.data() + copied, bytes);
        copied += bytes;
    }
    if (copied != chunk_header.size()) {
        fail("chunk reader ring buffer could not fit oversized chunk header");
    }
    state.onBytesReceived(copied);

    if (!state.parseFromRingBuffer()) {
        fail("oversized chunk size line did not complete with an error");
    }

    auto result = state.takeResult();
    if (result) {
        fail("oversized chunk size line completed successfully");
    }
    if (result.error().code() != kRequestEntityTooLarge) {
        fail("oversized chunk size line returned wrong error: " + result.error().message());
    }
    if (!chunk_data.empty()) {
        fail("oversized chunk size line appended body bytes before returning error");
    }
}

void verifyChunkReaderStreamsChunkLargerThanRingBuffer()
{
    RingBuffer ring_buffer{4096};
    HttpReaderSetting setting;
    setting.setMaxBodySize(kChunkBodySize + 1024);
    std::string chunk_data;
    galay::http::detail::HttpChunkReadState state(ring_buffer, setting, chunk_data);

    const std::string body(kChunkBodySize, 'C');
    const std::string raw = Chunk::toChunk(body, false) + Chunk::toChunk(std::string{}, true);

    size_t offset = 0;
    size_t read_windows = 0;
    while (true) {
        if (state.parseFromRingBuffer()) {
            auto result = state.takeResult();
            if (!result) {
                fail("large chunk parse returned error: " + result.error().message());
            }
            if (!result.value()) {
                fail("large chunk parse completed before the last chunk");
            }
            if (chunk_data.size() != body.size()) {
                fail("large chunk body size mismatch: " + std::to_string(chunk_data.size()));
            }
            if (chunk_data != body) {
                fail("large chunk body content mismatch");
            }
            if (offset != raw.size()) {
                fail("large chunk parser completed before all peer bytes were supplied");
            }
            if (read_windows <= 1) {
                fail("large chunk test did not cross the RingBuffer window");
            }
            return;
        }

        if (offset >= raw.size()) {
            fail("large chunk parser still needs bytes after full input");
        }

        if (!state.prepareRecvWindow()) {
            fail("large chunk reader could not provide another receive window");
        }

        const struct iovec* windows = state.recvIovecsData();
        const size_t window_count = state.recvIovecsCount();
        size_t copied = 0;
        for (size_t i = 0; i < window_count && offset + copied < raw.size(); ++i) {
            const size_t bytes = std::min(
                windows[i].iov_len,
                raw.size() - offset - copied);
            std::memcpy(windows[i].iov_base, raw.data() + offset + copied, bytes);
            copied += bytes;
        }
        if (copied == 0) {
            fail("large chunk receive window could not accept more bytes");
        }

        state.onBytesReceived(copied);
        offset += copied;
        ++read_windows;
        if (read_windows >= 1024) {
            fail("large chunk parse did not converge");
        }
    }
}

void verifyChunkedRequestBodyLargerThanRingBuffer()
{
    RingBuffer ring_buffer{4096};
    HttpReaderSetting setting;
    setting.setMaxBodySize(kChunkBodySize + 1024);
    HttpRequest request;
    galay::http::detail::HttpRequestReadState state(ring_buffer, setting, request);

    const std::string body(kChunkBodySize, 'R');
    const std::string raw =
        "POST /chunked HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n" +
        Chunk::toChunk(body, false) +
        Chunk::toChunk(std::string{}, true);

    size_t offset = 0;
    size_t read_windows = 0;
    while (true) {
        if (state.parseFromRingBuffer()) {
            auto result = state.takeResult();
            if (!result) {
                fail("large chunked request parse returned error: " + result.error().message());
            }
            if (!request.isComplete()) {
                fail("large chunked request did not complete");
            }
            if (request.bodyStr() != body) {
                fail("large chunked request body mismatch");
            }
            if (offset != raw.size()) {
                fail("large chunked request completed before all peer bytes were supplied");
            }
            if (read_windows <= 1) {
                fail("large chunked request did not cross the RingBuffer window");
            }
            return;
        }

        if (offset >= raw.size()) {
            fail("large chunked request still needs bytes after full input");
        }

        if (!state.prepareRecvWindow()) {
            fail("large chunked request reader could not provide another receive window");
        }

        const struct iovec* windows = state.recvIovecsData();
        const size_t window_count = state.recvIovecsCount();
        size_t copied = 0;
        for (size_t i = 0; i < window_count && offset + copied < raw.size(); ++i) {
            const size_t bytes = std::min(
                windows[i].iov_len,
                raw.size() - offset - copied);
            std::memcpy(windows[i].iov_base, raw.data() + offset + copied, bytes);
            copied += bytes;
        }
        if (copied == 0) {
            fail("large chunked request receive window could not accept more bytes");
        }

        state.onBytesReceived(copied);
        offset += copied;
        ++read_windows;
        if (read_windows >= 1024) {
            fail("large chunked request parse did not converge");
        }
    }
}

Task<void> largeUploadConnHandler(HttpConn conn)
{
    HttpReaderSetting reader_setting;
    reader_setting.setMaxBodySize(kBodySize + 1024);
    auto reader = conn.getReader(reader_setting);

    HttpRequest req;
    auto read_result = co_await reader.getRequest(req);
    if (!read_result) {
        std::cerr << "[T79] read request failed: " << read_result.error().message() << "\n";
        co_await conn.close();
        co_return;
    }

    const auto& body = req.bodyStr();
    if (body.size() != kBodySize) {
        std::cerr << "[T79] handler received body size " << body.size()
                  << ", expected " << kBodySize << "\n";
    }

    auto response = Http1_1ResponseBuilder::ok()
        .text("received=" + std::to_string(body.size()))
        .buildMove();

    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);
    if (!result) {
        std::cerr << "[T79] send response failed: " << result.error().message() << "\n";
    }
    co_await conn.close();
    co_return;
}

Task<void> rejectOversizeConnHandler(HttpConn conn)
{
    HttpReaderSetting reader_setting;
    reader_setting.setMaxBodySize(1024);
    auto reader = conn.getReader(reader_setting);

    HttpRequest req;
    auto read_result = co_await reader.getRequest(req);
    auto writer = conn.getWriter();

    if (!read_result && read_result.error().code() == kRequestEntityTooLarge) {
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::PayloadTooLarge_413)
            .text("too large")
            .buildMove();
        (void) co_await writer.sendResponse(response);
        co_await conn.close();
        co_return;
    }

    auto response = Http1_1ResponseBuilder()
        .status(HttpStatusCode::InternalServerError_500)
        .text("unexpected read result")
        .buildMove();
    (void) co_await writer.sendResponse(response);
    co_await conn.close();
    co_return;
}

Task<void> largeChunkConnHandler(HttpConn conn)
{
    HttpReaderSetting reader_setting;
    reader_setting.setMaxBodySize(kBodySize + 1024);
    auto reader = conn.getReader(reader_setting);

    std::string chunk_data;
    auto read_result = co_await reader.getChunk(chunk_data);
    auto writer = conn.getWriter();

    if (read_result && read_result.value() && chunk_data.size() == kBodySize) {
        auto response = Http1_1ResponseBuilder::ok()
            .text("chunked=" + std::to_string(chunk_data.size()))
            .buildMove();
        (void) co_await writer.sendResponse(response);
        co_await conn.close();
        co_return;
    }

    if (!read_result) {
        std::cerr << "[T79] read chunk failed: " << read_result.error().message() << "\n";
    } else {
        std::cerr << "[T79] read chunk returned last=" << read_result.value()
                  << ", size=" << chunk_data.size() << "\n";
    }

    auto response = Http1_1ResponseBuilder()
        .status(HttpStatusCode::InternalServerError_500)
        .text("unexpected chunk result")
        .buildMove();
    (void) co_await writer.sendResponse(response);
    co_await conn.close();
    co_return;
}

Task<void> rejectOversizeChunkConnHandler(HttpConn conn)
{
    HttpReaderSetting reader_setting;
    reader_setting.setMaxBodySize(1024);
    auto reader = conn.getReader(reader_setting);

    std::string chunk_data;
    auto read_result = co_await reader.getChunk(chunk_data);
    auto writer = conn.getWriter();

    if (!read_result && read_result.error().code() == kRequestEntityTooLarge) {
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::PayloadTooLarge_413)
            .text("chunk too large")
            .buildMove();
        (void) co_await writer.sendResponse(response);
        co_await conn.close();
        co_return;
    }

    auto response = Http1_1ResponseBuilder()
        .status(HttpStatusCode::InternalServerError_500)
        .text("unexpected chunk read result")
        .buildMove();
    (void) co_await writer.sendResponse(response);
    co_await conn.close();
    co_return;
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

        std::cerr << "[T79] connect failed, errno=" << err << "\n";
        std::abort();
    }

    std::cerr << "[T79] connect retry exhausted, port=" << port << "\n";
    std::abort();
}

void sendAll(int fd, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            std::cerr << "[T79] send failed after " << sent
                      << " bytes, errno=" << errno << " (" << std::strerror(errno) << ")\n";
            std::abort();
        }
        sent += static_cast<size_t>(n);
    }
}

std::string recvUntilClosed(int fd)
{
    std::string response;
    char buffer[8192];
    while (true) {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            std::cerr << "[T79] recv failed, errno=" << errno
                      << " (" << std::strerror(errno) << ")\n";
            std::abort();
        }
        response.append(buffer, static_cast<size_t>(n));
    }
    return response;
}

void assertContains(const std::string& text, const std::string& expected)
{
    if (text.find(expected) == std::string::npos) {
        std::cerr << "[T79] expected substring not found: " << expected << "\n";
        std::cerr << "[T79] actual response prefix:\n" << text.substr(0, 500) << "\n";
        std::abort();
    }
}

void verifyCoAwaitRequestReturnsEntityTooLarge()
{
    uint16_t port = pickFreePort();
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build());

    g_stage = 2;
    server.start(rejectOversizeConnHandler);

    g_stage = 3;
    int fd = connectWithRetry(port);

    const std::string request =
        "PUT /upload HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 2048\r\n"
        "Connection: close\r\n"
        "\r\n";

    g_stage = 4;
    sendAll(fd, request);
    g_stage = 5;
    std::string response = recvUntilClosed(fd);
    ::close(fd);

    g_stage = 6;
    server.stop();

    assertContains(response, "HTTP/1.1 413 Payload Too Large");
    assertContains(response, "too large");
}

void verifyCoAwaitChunkReaderReturnsEntityTooLarge()
{
    uint16_t port = pickFreePort();
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build());

    g_stage = 2;
    server.start(rejectOversizeChunkConnHandler);

    g_stage = 3;
    int fd = connectWithRetry(port);

    g_stage = 4;
    sendAll(fd, "800\r\n");
    g_stage = 5;
    std::string response = recvUntilClosed(fd);
    ::close(fd);

    g_stage = 6;
    server.stop();

    assertContains(response, "HTTP/1.1 413 Payload Too Large");
    assertContains(response, "chunk too large");
}

void verifyCoAwaitChunkReaderStreamsLargeChunk()
{
    uint16_t port = pickFreePort();
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build());

    g_stage = 2;
    server.start(largeChunkConnHandler);

    g_stage = 3;
    int fd = connectWithRetry(port);

    std::string chunk_stream = makeLargeChunkStream();
    g_stage = 4;
    sendAll(fd, chunk_stream);
    g_stage = 5;
    std::string response = recvUntilClosed(fd);
    ::close(fd);

    g_stage = 6;
    server.stop();

    assertContains(response, "HTTP/1.1 200 OK");
    assertContains(response, "chunked=" + std::to_string(kBodySize));
}

} // namespace

int main()
{
    ::signal(SIGALRM, alarmHandler);
    ::alarm(20);

    g_stage = 1;
    verifyHugeContentLengthReturnsEntityTooLarge();
    verifyChunkReaderReturnsEntityTooLarge();
    verifyChunkReaderRejectsOversizeFromSizeLine();
    verifyChunkReaderStreamsChunkLargerThanRingBuffer();
    verifyChunkedRequestBodyLargerThanRingBuffer();
    verifyCoAwaitRequestReturnsEntityTooLarge();
    verifyCoAwaitChunkReaderReturnsEntityTooLarge();
    verifyCoAwaitChunkReaderStreamsLargeChunk();

    std::string request = makeLargePutRequest();
    g_stage = 1;
    const size_t read_windows = countReadWindowsForDefaultHttpConn(request);
    if (read_windows < 64) {
        std::cerr << "[T79] test request only needs " << read_windows
                  << " read windows; it must cross the old 64-read state-machine limit\n";
        std::abort();
    }

    uint16_t port = pickFreePort();

    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build());
    g_stage = 2;
    server.start(largeUploadConnHandler);

    g_stage = 3;
    int fd = connectWithRetry(port);

    g_stage = 4;
    sendAll(fd, request);
    g_stage = 5;
    std::string response = recvUntilClosed(fd);
    ::close(fd);

    g_stage = 6;
    server.stop();
    ::alarm(0);

    assertContains(response, "HTTP/1.1 200 OK");
    assertContains(response, "received=" + std::to_string(kBodySize));

    std::cout << "T79-LargePutRequestBody PASS\n";
    return 0;
}
