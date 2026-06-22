/**
 * @file t9_http_server_security_lifecycle.cc
 * @brief 覆盖 HTTP MCP per-connection 初始化、传输限制和 stop 生命周期。
 */

#include <galay/cpp/galay-mcp/server/http_server.h>
#include <galay/cpp/galay-mcp/common/mcp_policy.h>

#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

bool require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

uint16_t pickFreePort()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket failed");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("inet_pton failed");
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("bind failed");
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        throw std::runtime_error("getsockname failed");
    }
    const auto port = static_cast<uint16_t>(ntohs(addr.sin_port));
    ::close(fd);
    return port;
}

int connectWithRetry(uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        return -1;
    }

    for (int attempt = 0; attempt < 100; ++attempt) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            timeval timeout{};
            timeout.tv_sec = 2;
            (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
}

bool canConnect(uint16_t port)
{
    const int fd = connectWithRetry(port);
    if (fd >= 0) {
        ::close(fd);
        return true;
    }
    return false;
}

void sendAll(int fd, const std::string& data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
        }
        sent += static_cast<std::size_t>(n);
    }
}

std::optional<std::size_t> contentLength(std::string_view header)
{
    constexpr std::string_view kName = "Content-Length: ";
    const auto pos = header.find(kName);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto begin = pos + kName.size();
    const auto end = header.find("\r\n", begin);
    return static_cast<std::size_t>(std::stoull(std::string(header.substr(begin, end - begin))));
}

std::string readHttpResponse(int fd)
{
    std::string response;
    char buffer[4096];
    std::optional<std::size_t> expectedSize;
    while (true) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
        }
        response.append(buffer, static_cast<std::size_t>(n));
        const auto headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos && !expectedSize.has_value()) {
            expectedSize = headerEnd + 4 + contentLength(std::string_view(response).substr(0, headerEnd + 2)).value();
        }
        if (expectedSize.has_value() && response.size() >= *expectedSize) {
            return response;
        }
    }
}

std::string makePost(std::string_view body, std::string_view connection = "keep-alive")
{
    std::string request;
    request.reserve(body.size() + 160);
    request += "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nConnection: ";
    request += connection;
    request += "\r\nContent-Length: ";
    request += std::to_string(body.size());
    request += "\r\n\r\n";
    request.append(body.data(), body.size());
    return request;
}

std::string bodyOf(std::string_view response)
{
    const auto pos = response.find("\r\n\r\n");
    if (pos == std::string_view::npos) {
        return {};
    }
    return std::string(response.substr(pos + 4));
}

std::string initializeBody(int id)
{
    return std::string(R"({"jsonrpc":"2.0","id":)") + std::to_string(id) +
           R"(,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})";
}

std::string postOnce(uint16_t port, std::string_view body)
{
    const int fd = connectWithRetry(port);
    if (fd < 0) {
        throw std::runtime_error("connect failed");
    }
    sendAll(fd, makePost(body, "close"));
    const auto response = readHttpResponse(fd);
    ::close(fd);
    return bodyOf(response);
}

bool waitUntilListening(uint16_t port)
{
    for (int i = 0; i < 100; ++i) {
        if (canConnect(port)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

int main()
{
    const uint16_t port = pickFreePort();
    galay::mcp::McpHttpServer server("127.0.0.1", port, 1, 1);
    galay::mcp::McpProductionPolicy policy;
    policy.transport.max_http_body_bytes = 512;
    policy.transport.max_response_bytes = 512;
    policy.transport.max_keep_alive_requests = 2;
    server.setProductionPolicy(policy);

    server.addTool("large", "large result", "{}", [](const galay::mcp::JsonElement&,
                                                      std::expected<galay::mcp::JsonString, galay::mcp::McpError>& result)
        -> galay::kernel::Task<void> {
        result = std::string(1024, 'x');
        co_return;
    });

    std::thread serverThread([&server] {
        server.start();
    });

    if (!require(waitUntilListening(port), "HTTP MCP server did not start listening")) {
        server.stop();
        serverThread.join();
        return 1;
    }

    const auto initResponse = postOnce(port, initializeBody(1));
    if (!require(initResponse.find("\"result\"") != std::string::npos,
                 "initialize request did not succeed")) {
        server.stop();
        serverThread.join();
        return 1;
    }

    const auto secondConnectionList =
        postOnce(port, R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})");
    if (!require(secondConnectionList.find("Not initialized") != std::string::npos,
                 "tools/list on a different HTTP connection reused global initialization")) {
        server.stop();
        serverThread.join();
        return 1;
    }

    const auto oversizedBody = postOnce(port, std::string(513, 'x'));
    if (!require(oversizedBody.find("Payload too large") != std::string::npos,
                 "oversized HTTP body was not rejected at MCP boundary")) {
        server.stop();
        serverThread.join();
        return 1;
    }

    const int largeFd = connectWithRetry(port);
    if (!require(largeFd >= 0, "large-response connect failed")) {
        server.stop();
        serverThread.join();
        return 1;
    }
    sendAll(largeFd, makePost(initializeBody(5)));
    (void)readHttpResponse(largeFd);
    sendAll(largeFd, makePost(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"large","arguments":{}}})"));
    const auto largeResponseBody = bodyOf(readHttpResponse(largeFd));
    ::close(largeFd);
    if (!require(largeResponseBody.find("Payload too large") != std::string::npos,
                 "oversized MCP response was not rejected before HTTP send")) {
        server.stop();
        serverThread.join();
        return 1;
    }

    const int fd = connectWithRetry(port);
    if (!require(fd >= 0, "keep-alive connect failed")) {
        server.stop();
        serverThread.join();
        return 1;
    }
    sendAll(fd, makePost(initializeBody(3)));
    (void)readHttpResponse(fd);
    sendAll(fd, makePost(R"({"jsonrpc":"2.0","id":4,"method":"tools/list","params":{}})"));
    (void)readHttpResponse(fd);
    sendAll(fd, makePost(R"({"jsonrpc":"2.0","id":7,"method":"tools/list","params":{}})"));
    const auto keepAliveBody = bodyOf(readHttpResponse(fd));
    ::close(fd);
    if (!require(keepAliveBody.find("Keep-alive request limit exceeded") != std::string::npos,
                 "keep-alive request cap was not enforced")) {
        server.stop();
        serverThread.join();
        return 1;
    }

    server.stop();
    serverThread.join();

    if (!require(!canConnect(port), "McpHttpServer::stop left the underlying HTTP server accepting connections")) {
        return 1;
    }

    std::cout << "T9-HttpServerSecurityLifecycle PASS\n";
    return 0;
}
