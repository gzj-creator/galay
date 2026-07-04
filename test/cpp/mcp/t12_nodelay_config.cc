#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-mcp/client/client.h>
#include <galay/cpp/galay-mcp/client/http_transport.h>
#include <galay/cpp/galay-mcp/server/http_server.h>

#ifndef GALAY_PROJECT_SOURCE_DIR
#define GALAY_PROJECT_SOURCE_DIR "."
#endif

namespace {

[[noreturn]] void fail(const char* message)
{
    std::cerr << "[T12] " << message << "\n";
    std::abort();
}

void require(bool condition, const char* message)
{
    if (!condition) {
        fail(message);
    }
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) {
        std::cerr << "[T12] failed to open " << path << "\n";
        std::abort();
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

void test_client_config_surface()
{
    galay::mcp::McpHttpClientConfig default_config;
    require(default_config.tcp_no_delay, "McpHttpClientConfig should enable TCP_NODELAY by default");

    galay::mcp::McpHttpClientConfig disabled_config;
    disabled_config.url = "http://127.0.0.1:8080/mcp";
    disabled_config.tcp_no_delay = false;
    require(!disabled_config.tcp_no_delay, "McpHttpClientConfig should support disabling TCP_NODELAY");
}

void test_http_transport_passes_client_config()
{
    galay::kernel::Runtime runtime;

    galay::mcp::McpHttpClientConfig default_config;
    default_config.url = "http://127.0.0.1:8080/mcp";
    [[maybe_unused]] galay::mcp::detail::HttpClientTransport default_transport(runtime, default_config);

    galay::mcp::McpHttpClientConfig disabled_config;
    disabled_config.url = "http://127.0.0.1:8080/mcp";
    disabled_config.tcp_no_delay = false;
    [[maybe_unused]] galay::mcp::detail::HttpClientTransport disabled_transport(runtime, disabled_config);
}

void test_http_server_config_surface()
{
    [[maybe_unused]] galay::mcp::McpHttpServer default_server("127.0.0.1", 8080, 1, 0);
    [[maybe_unused]] galay::mcp::McpHttpServer disabled_server("127.0.0.1", 8081, 1, 0, false);
}

void test_passthrough_source_boundaries()
{
    const std::filesystem::path source_root = GALAY_PROJECT_SOURCE_DIR;
    const auto client_source = readFile(source_root / "src/cpp/galay-mcp/client/client.cc");
    const auto transport_source = readFile(source_root / "src/cpp/galay-mcp/client/http_transport.cc");
    const auto server_source = readFile(source_root / "src/cpp/galay-mcp/server/http_server.cc");

    require(contains(client_source, "HttpClientTransport>(runtime, std::move(config))"),
            "McpClient should pass the full HTTP client config into HttpClientTransport");
    require(contains(transport_source, ".tcpNoDelay(config.tcp_no_delay)"),
            "HttpClientTransport should pass TCP_NODELAY into HttpClientBuilder");
    require(contains(server_source, "config.tcp_no_delay = m_tcpNoDelay;"),
            "McpHttpServer::start should pass TCP_NODELAY into HttpServerConfig");
}

} // namespace

int main()
{
    test_client_config_surface();
    test_http_transport_passes_client_config();
    test_http_server_config_surface();
    test_passthrough_source_boundaries();

    std::cout << "T12-McpNoDelayConfig PASS\n";
    return 0;
}
