/**
 * @file t17_v2_http_server.cc
 * @brief MCP 2026-07-28 Streamable HTTP header contract.
 */

#include <galay/cpp/galay-mcp/v2/server/http_server.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

bool require(bool value, std::string_view message)
{
    if (!value) std::cerr << message << '\n';
    return value;
}

uint16_t port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        ::close(fd);
        return 0;
    }
    address.sin_port = 0;
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (fd >= 0) ::close(fd);
        return 0;
    }
    socklen_t length = sizeof(address);
    const bool readOk = ::listen(fd, 1) == 0 &&
                        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    const auto value = static_cast<uint16_t>(ntohs(address.sin_port));
    ::close(fd);
    return readOk ? value : 0;
}

int connectTo(uint16_t value)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    (void)::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    address.sin_port = htons(value);
    for (int attempt = 0; attempt != 100; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            timeval timeout{2, 0};
            (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            return fd;
        }
        if (fd >= 0) ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
}

void sendAll(int fd, std::string_view value)
{
    std::size_t offset = 0;
    while (offset != value.size()) {
        const auto written = ::send(fd, value.data() + offset, value.size() - offset, 0);
        if (written <= 0) return;
        offset += static_cast<std::size_t>(written);
    }
}

std::string exchange(uint16_t portValue, std::string_view body, std::string_view extraHeaders)
{
    const int fd = connectTo(portValue);
    if (fd < 0) return {};
    std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nConnection: close\r\n";
    request.append(extraHeaders.data(), extraHeaders.size());
    request += "Content-Length: ";
    request += std::to_string(body.size());
    request += "\r\n\r\n";
    request.append(body.data(), body.size());
    sendAll(fd, request);
    std::string response;
    char buffer[4096];
    while (true) {
        const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        response.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return response;
}

std::string body(std::string_view response)
{
    const auto offset = response.find("\r\n\r\n");
    return offset == std::string_view::npos ? std::string{} : std::string(response.substr(offset + 4));
}

std::string recvUntil(int fd, std::string_view marker)
{
    std::string response;
    char buffer[4096];
    while (response.find(marker) == std::string::npos) {
        const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        response.append(buffer, static_cast<std::size_t>(n));
    }
    return response;
}

int openListen(uint16_t portValue, std::string_view bodyValue)
{
    const int fd = connectTo(portValue);
    if (fd < 0) return -1;
    std::string request =
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json, text/event-stream\r\n"
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Method: subscriptions/listen\r\n"
        "Connection: close\r\nContent-Length: ";
    request += std::to_string(bodyValue.size());
    request += "\r\n\r\n";
    request.append(bodyValue.data(), bodyValue.size());
    sendAll(fd, request);
    return fd;
}

std::string makeBody(std::string_view method, std::string_view fields)
{
    galay::mcp::v2::RequestMeta meta;
    auto params = galay::mcp::v2::makeRequestParams(meta, fields);
    if (!params) return {};
    galay::mcp::v2::JsonRpcRequest request;
    request.id = 1;
    request.method = std::string(method);
    request.params = std::move(params.value());
    return request.toJson();
}

} // namespace

int main()
{
    const uint16_t selectedPort = port();
    if (!require(selectedPort != 0, "failed to select test port")) return 1;

    galay::mcp::v2::McpHttpServer server("127.0.0.1", selectedPort, 1, 1);
    server.addTool("echo", "Echo",
                   R"({"type":"object","properties":{"region":{"type":"string","x-mcp-header":"Region"}}})",
                   [](const galay::mcp::JsonElement&,
                      std::expected<galay::mcp::JsonString, galay::mcp::McpError>& result)
                       -> galay::kernel::Task<void> {
                       result = std::string("hello");
                       co_return;
                   });
    server.addResource("mem://hello", "hello", "Hello", "text/plain",
                       [](const std::string&,
                          std::expected<std::string, galay::mcp::McpError>& result)
                           -> galay::kernel::Task<void> {
                           result = std::string("hello");
                           co_return;
                       });
    std::thread serverThread([&server] { server.start(); });

    const int probe = connectTo(selectedPort);
    if (!require(probe >= 0, "v2 HTTP server did not start")) {
        server.stop();
        serverThread.join();
        return 1;
    }
    ::close(probe);

    const auto discover = makeBody(galay::mcp::v2::Methods::SERVER_DISCOVER, "{}");
    const auto valid = exchange(selectedPort,
                                discover,
                                "Accept: application/json, text/event-stream\r\n"
                                "MCP-Protocol-Version: 2026-07-28\r\n"
                                "Mcp-Method: server/discover\r\n");
    if (!require(valid.find("200 OK") != std::string::npos &&
                     body(valid).find("supportedVersions") != std::string::npos,
                 "valid v2 HTTP discover request failed")) {
        server.stop(); serverThread.join(); return 1;
    }

    const auto missingAccept = exchange(selectedPort,
                                        discover,
                                        "MCP-Protocol-Version: 2026-07-28\r\n"
                                        "Mcp-Method: server/discover\r\n");
    if (!require(missingAccept.find("400 Bad Request") != std::string::npos &&
                     body(missingAccept).find("-32020") != std::string::npos,
                 "missing Accept header did not return HeaderMismatch")) {
        server.stop(); serverThread.join(); return 1;
    }

    const auto versionMismatch = exchange(selectedPort,
                                          discover,
                                          "Accept: application/json, text/event-stream\r\n"
                                          "MCP-Protocol-Version: 2024-11-05\r\n"
                                          "Mcp-Method: server/discover\r\n");
    if (!require(versionMismatch.find("400 Bad Request") != std::string::npos &&
                     body(versionMismatch).find("-32020") != std::string::npos,
                 "version header mismatch did not return HeaderMismatch")) {
        server.stop(); serverThread.join(); return 1;
    }

    const auto call = makeBody(galay::mcp::v2::Methods::TOOLS_CALL,
                               R"({"name":"echo","arguments":{}})");
    const auto nameMismatch = exchange(selectedPort,
                                       call,
                                       "Accept: application/json, text/event-stream\r\n"
                                       "MCP-Protocol-Version: 2026-07-28\r\n"
                                       "Mcp-Method: tools/call\r\n"
                                       "Mcp-Name: wrong\r\n");
    if (!require(nameMismatch.find("400 Bad Request") != std::string::npos &&
                     body(nameMismatch).find("-32020") != std::string::npos,
                 "Mcp-Name mismatch did not return HeaderMismatch")) {
        server.stop(); serverThread.join(); return 1;
    }

    const auto headerCall = makeBody(galay::mcp::v2::Methods::TOOLS_CALL,
                                     R"({"name":"echo","arguments":{"region":"us west"}})");
    const auto missingParameterHeader = exchange(
        selectedPort, headerCall,
        "Accept: application/json, text/event-stream\r\n"
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Method: tools/call\r\n"
        "Mcp-Name: echo\r\n");
    if (!require(missingParameterHeader.find("400 Bad Request") != std::string::npos &&
                     body(missingParameterHeader).find("-32020") != std::string::npos,
                 "missing mirrored parameter header did not return HeaderMismatch")) {
        server.stop(); serverThread.join(); return 1;
    }

    const auto validParameterHeader = exchange(
        selectedPort, headerCall,
        "Accept: application/json, text/event-stream\r\n"
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Method: tools/call\r\n"
        "Mcp-Name: echo\r\n"
        "Mcp-Param-Region: =?base64?dXMgd2VzdA==?=\r\n");
    if (!require(validParameterHeader.find("200 OK") != std::string::npos &&
                     body(validParameterHeader).find("hello") != std::string::npos,
                 "valid mirrored parameter header was rejected")) {
        server.stop(); serverThread.join(); return 1;
    }

    const auto invalidOrigin = exchange(selectedPort,
                                        discover,
                                        "Accept: application/json, text/event-stream\r\n"
                                        "MCP-Protocol-Version: 2026-07-28\r\n"
                                        "Mcp-Method: server/discover\r\n"
                                        "Origin: https://attacker.example\r\n");
    if (!require(invalidOrigin.find("403 Forbidden") != std::string::npos,
                 "invalid Origin was not rejected with HTTP 403")) {
        server.stop(); serverThread.join(); return 1;
    }

    const auto listen = makeBody(
        galay::mcp::v2::Methods::SUBSCRIPTIONS_LISTEN,
        R"({"notifications":{"toolsListChanged":true,"promptsListChanged":true,"resourcesListChanged":true,"resourceSubscriptions":["mem://hello"]}})");
    const int listenFd = openListen(selectedPort, listen);
    if (!require(listenFd >= 0, "failed to open subscriptions/listen stream")) {
        server.stop(); serverThread.join(); return 1;
    }
    const auto acknowledged = recvUntil(
        listenFd, "notifications/subscriptions/acknowledged");
    if (!require(acknowledged.find("200 OK") != std::string::npos &&
                     acknowledged.find("content-type: text/event-stream") !=
                         std::string::npos &&
                     acknowledged.find("transfer-encoding: chunked") !=
                         std::string::npos &&
                     acknowledged.find("x-accel-buffering: no") !=
                         std::string::npos &&
                     acknowledged.find("\"toolsListChanged\":true") !=
                         std::string::npos &&
                     acknowledged.find("promptsListChanged") ==
                         std::string::npos,
                 "listen stream did not acknowledge only supported filters")) {
        ::close(listenFd);
        server.stop(); serverThread.join(); return 1;
    }
    if (!require(server.notifyToolsListChanged() == 1 &&
                     server.notifyPromptsListChanged() == 0 &&
                     server.notifyResourcesListChanged() == 1 &&
                     server.notifyResourceUpdated("mem://other") == 0 &&
                     server.notifyResourceUpdated("mem://hello/child") == 0 &&
                     server.notifyResourceUpdated("mem://hello") == 1,
                 "subscription filtering or delivery count is incorrect")) {
        ::close(listenFd);
        server.stop(); serverThread.join(); return 1;
    }
    const auto changed = recvUntil(listenFd, "notifications/tools/list_changed");
    if (!require(changed.find(
                     "\"io.modelcontextprotocol/subscriptionId\":1") !=
                     std::string::npos,
                 "listen stream notification is missing its subscription id")) {
        ::close(listenFd);
        server.stop(); serverThread.join(); return 1;
    }
    ::close(listenFd);

    const int shutdownListenFd = openListen(selectedPort, listen);
    if (!require(shutdownListenFd >= 0 &&
                     recvUntil(shutdownListenFd,
                               "notifications/subscriptions/acknowledged")
                         .find("notifications/subscriptions/acknowledged") !=
                         std::string::npos,
                 "second listen stream was not acknowledged")) {
        if (shutdownListenFd >= 0) ::close(shutdownListenFd);
        server.stop(); serverThread.join(); return 1;
    }

    server.stop();
    const auto gracefulClose = recvUntil(shutdownListenFd, "0\r\n\r\n");
    ::close(shutdownListenFd);
    serverThread.join();
    if (!require(gracefulClose.find("\"resultType\":\"complete\"") !=
                         std::string::npos &&
                     gracefulClose.find(
                         "\"io.modelcontextprotocol/subscriptionId\":1") !=
                         std::string::npos,
                 "server shutdown did not close the listen stream gracefully")) {
        return 1;
    }
    std::cout << "T17-V2HttpServer PASS\n";
    return 0;
}
