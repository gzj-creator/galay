#include <galay/cpp/galay-http/server/http_router.h>
#include <galay/cpp/galay-http/server/http_server.h>
#include <galay/cpp/galay-http/builder/http_builder.h>

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace galay::http;
using namespace galay::kernel;

namespace {

Task<void> upstreamEcho(HttpConn& conn, HttpRequest req)
{
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "E2E-Upstream/1.0")
        .text("Echo: " + req.getBodyStr())
        .buildMove();

    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);
    if (!result) {
        std::cerr << "[upstreamEcho] [send-fail] " << result.error().message() << "\n";
    }
    co_return;
}

Task<void> upstreamCatchAll(HttpConn& conn, HttpRequest req)
{
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "E2E-Upstream/1.0")
        .text("UPSTREAM:" + req.header().uri())
        .buildMove();

    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);
    if (!result) {
        std::cerr << "[upstreamCatchAll] [send-fail] " << result.error().message() << "\n";
    }
    co_return;
}

Task<void> upstreamHeaders(HttpConn& conn, HttpRequest req)
{
    auto& headers = req.header().headerPairs();
    const std::string host = headers.getValue("Host");
    const std::string xff = headers.getValue("X-Forwarded-For");
    const std::string xreal = headers.getValue("X-Real-IP");
    const std::string xfproto = headers.getValue("X-Forwarded-Proto");
    const std::string xfhost = headers.getValue("X-Forwarded-Host");

    std::string body;
    body.reserve(256);
    body += "Host=" + host + "\n";
    body += "X-Forwarded-For=" + xff + "\n";
    body += "X-Real-IP=" + xreal + "\n";
    body += "X-Forwarded-Proto=" + xfproto + "\n";
    body += "X-Forwarded-Host=" + xfhost + "\n";

    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "E2E-Upstream/1.0")
        .text(body)
        .buildMove();

    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);
    if (!result) {
        std::cerr << "[upstreamHeaders] [send-fail] " << result.error().message() << "\n";
    }
    co_return;
}

Task<void> upstreamConnPort(HttpConn& conn, HttpRequest req)
{
    (void)req;
    uint16_t remote_port = 0;

    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    int rc = ::getpeername(conn.getSocket().handle().fd, reinterpret_cast<sockaddr*>(&addr), &len);
    if (rc == 0 && addr.ss_family == AF_INET) {
        auto* in = reinterpret_cast<sockaddr_in*>(&addr);
        remote_port = ntohs(in->sin_port);
    }

    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "E2E-Upstream/1.0")
        .text("UPSTREAM_CONN_PORT:" + std::to_string(remote_port))
        .buildMove();

    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);
    if (!result) {
        std::cerr << "[upstreamConnPort] [send-fail] " << result.error().message() << "\n";
    }
    co_return;
}

Task<void> upstreamStream(HttpConn& conn, HttpRequest req)
{
    (void)req;
    auto writer = conn.getWriter();

    auto send_blob = [&writer](std::string blob) -> Task<void> {
        auto result = co_await writer.send(std::move(blob));
        if (!result) {
            co_return;
        }
        co_return;
    };

    co_await send_blob(
        "HTTP/1.1 200 OK\r\n"
        "Server: E2E-Upstream/1.0\r\n"
        "Content-Type: text/plain\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n");
    co_await send_blob("5\r\nhello\r\n");
    co_await send_blob("5\r\nworld\r\n");
    co_await send_blob("0\r\n\r\n");
    co_return;
}

std::string resolveStaticDir()
{
    const std::string candidates[] = {
        "./test/static_files",
        "../test/static_files",
        "../../test/static_files"
    };

    for (const auto& path : candidates) {
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return "./test/static_files";
}

std::string sendRawHttp(const std::string& request, uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    int pton_ok = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    assert(pton_ok == 1);

    int fd = -1;
    for (int attempt = 0; attempt < 100; ++attempt) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);

        int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc == 0) {
            break;
        }

        int err = errno;
        ::close(fd);
        fd = -1;

        if (err == ECONNREFUSED || err == ETIMEDOUT || err == EHOSTUNREACH || err == ENETUNREACH) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        std::cerr << "connect failed, errno=" << err << "\n";
        std::abort();
    }

    if (fd < 0) {
        std::cerr << "connect retry exhausted, port=" << port << "\n";
        std::abort();
    }

    size_t sent = 0;
    while (sent < request.size()) {
        ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
        assert(n > 0);
        sent += static_cast<size_t>(n);
    }

    std::string response;
    char buffer[8192];
    while (true) {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) break;
        assert(n > 0);
        response.append(buffer, static_cast<size_t>(n));
    }

    ::close(fd);
    return response;
}

void assertContains(const std::string& text, const std::string& expected)
{
    if (text.find(expected) == std::string::npos) {
        std::cerr << "Expected substring not found: " << expected << "\n";
        std::cerr << "Actual response (first 500 chars):\n";
        std::cerr << text.substr(0, 500) << "\n";
        std::abort();
    }
}

void assertNotContains(const std::string& text, const std::string& unexpected)
{
    if (text.find(unexpected) != std::string::npos) {
        std::cerr << "Unexpected substring found: " << unexpected << "\n";
        std::cerr << "Actual response (first 500 chars):\n";
        std::cerr << text.substr(0, 500) << "\n";
        std::abort();
    }
}

std::string extractBody(const std::string& response)
{
    const std::string delimiter = "\r\n\r\n";
    size_t pos = response.find(delimiter);
    if (pos == std::string::npos) {
        return "";
    }
    return response.substr(pos + delimiter.size());
}

uint16_t parseConnPortFromResponse(const std::string& response)
{
    const std::string marker = "UPSTREAM_CONN_PORT:";
    std::string body = extractBody(response);
    size_t pos = body.find(marker);
    if (pos == std::string::npos) {
        std::cerr << "No connection marker in body: " << body << "\n";
        std::abort();
    }

    std::string value = body.substr(pos + marker.size());
    int port = std::atoi(value.c_str());
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid connection port: " << value << "\n";
        std::abort();
    }
    return static_cast<uint16_t>(port);
}

uint16_t pickFreePort()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    int pton_ok = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    assert(pton_ok == 1);

    int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(rc == 0);

    socklen_t len = sizeof(addr);
    rc = ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    assert(rc == 0);

    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

} // namespace

int main()
{
    uint16_t upstream_port = pickFreePort();
    uint16_t proxy_port = pickFreePort();
    while (proxy_port == upstream_port) {
        proxy_port = pickFreePort();
    }
    const std::string static_dir = resolveStaticDir();

    HttpRouter upstream_router;
    upstream_router.addHandler<HttpMethod::POST>("/echo", upstreamEcho);
    upstream_router.addHandler<HttpMethod::GET>("/headers", upstreamHeaders);
    upstream_router.addHandler<HttpMethod::GET>("/conn-port", upstreamConnPort);
    upstream_router.addHandler<HttpMethod::GET>("/stream", upstreamStream);
    upstream_router.addHandler<HttpMethod::GET>("/**", upstreamCatchAll);

    HttpServer upstream_server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(upstream_port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build());
    upstream_server.start(std::move(upstream_router));

    HttpRouter proxy_router;
    proxy_router.mount("/static", static_dir);
    proxy_router.proxy("/", "127.0.0.1", upstream_port);
    proxy_router.proxy("/raw", "127.0.0.1", upstream_port, ProxyMode::Raw);

    HttpServer proxy_server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(proxy_port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build());
    proxy_server.start(std::move(proxy_router));

    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    std::string req1 = Http1_1RequestBuilder::post("/echo")
        .host("127.0.0.1")
        .connection("close")
        .contentType("text/plain")
        .body("via proxy")
        .buildMove()
        .toString();
    std::string resp1 = sendRawHttp(req1, proxy_port);
    assertContains(resp1, "HTTP/1.1 200 OK");
    assertContains(resp1, "Echo: via proxy");

    std::string req2 = Http1_1RequestBuilder::get("/static/index.html")
        .host("127.0.0.1")
        .connection("close")
        .buildMove()
        .toString();
    std::string resp2 = sendRawHttp(req2, proxy_port);
    assertContains(resp2, "HTTP/1.1 200 OK");
    assertContains(resp2, "<!DOCTYPE html>");

    std::string req3 = Http1_1RequestBuilder::get("/static/not-found-xyz")
        .host("127.0.0.1")
        .connection("close")
        .buildMove()
        .toString();
    std::string resp3 = sendRawHttp(req3, proxy_port);
    assertContains(resp3, "HTTP/1.1 200 OK");
    assertContains(resp3, "UPSTREAM:/static/not-found-xyz");

    std::string req4 = Http1_1RequestBuilder::get("/headers")
        .host("proxy.demo.local")
        .header("X-Forwarded-For", "1.2.3.4")
        .connection("close")
        .buildMove()
        .toString();
    std::string resp4 = sendRawHttp(req4, proxy_port);
    assertContains(resp4, "HTTP/1.1 200 OK");
    assertContains(resp4, "Host=127.0.0.1:");
    assertContains(resp4, "X-Forwarded-For=1.2.3.4, 127.0.0.1");
    assertContains(resp4, "X-Real-IP=127.0.0.1");
    assertContains(resp4, "X-Forwarded-Proto=http");
    assertContains(resp4, "X-Forwarded-Host=proxy.demo.local");

    std::string req5 = Http1_1RequestBuilder::get("/conn-port")
        .host("127.0.0.1")
        .connection("close")
        .buildMove()
        .toString();
    std::string req6 = Http1_1RequestBuilder::get("/conn-port")
        .host("127.0.0.1")
        .connection("close")
        .buildMove()
        .toString();
    std::string resp5 = sendRawHttp(req5, proxy_port);
    std::string resp6 = sendRawHttp(req6, proxy_port);
    assertContains(resp5, "HTTP/1.1 200 OK");
    assertContains(resp6, "HTTP/1.1 200 OK");
    uint16_t conn_port_1 = parseConnPortFromResponse(resp5);
    uint16_t conn_port_2 = parseConnPortFromResponse(resp6);
    if (conn_port_1 != conn_port_2) {
        std::cerr << "Expected reused upstream connection port, got "
                  << conn_port_1 << " and " << conn_port_2 << "\n";
        std::abort();
    }

    std::string req7 = Http1_1RequestBuilder::get("/stream")
        .host("127.0.0.1")
        .connection("close")
        .buildMove()
        .toString();
    std::string resp7 = sendRawHttp(req7, proxy_port);
    assertContains(resp7, "HTTP/1.1 200 OK");
    assertContains(resp7, "Transfer-Encoding: chunked");
    assertContains(resp7, "5\r\nhello\r\n");
    assertContains(resp7, "5\r\nworld\r\n");
    assertContains(resp7, "0\r\n\r\n");
    assertNotContains(resp7, "Content-Length:");

    std::string req8 = Http1_1RequestBuilder::get("/raw/stream")
        .host("127.0.0.1")
        .connection("close")
        .buildMove()
        .toString();
    std::string resp8 = sendRawHttp(req8, proxy_port);
    assertContains(resp8, "HTTP/1.1 200 OK");
    assertContains(resp8, "Transfer-Encoding: chunked");
    assertContains(resp8, "5\r\nhello\r\n");
    assertContains(resp8, "5\r\nworld\r\n");
    assertContains(resp8, "0\r\n\r\n");
    assertNotContains(resp8, "Content-Length:");

    std::string req9 = Http1_1RequestBuilder::post("/raw/echo")
        .host("127.0.0.1")
        .connection("close")
        .contentType("application/json")
        .body("{\"q\":\"stream\"}")
        .buildMove()
        .toString();
    std::string resp9 = sendRawHttp(req9, proxy_port);
    assertContains(resp9, "HTTP/1.1 200 OK");
    assertContains(resp9, "Echo: {\"q\":\"stream\"}");

    proxy_server.stop();
    upstream_server.stop();

    std::cout << "T26-ProxyTryFilesE2E PASS\n";
    return 0;
}
