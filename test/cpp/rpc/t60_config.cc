#include <galay/cpp/galay-rpc/config/rpc_config_loader.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace galay::rpc;

namespace {

int expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << "\n";
        return 1;
    }
    return 0;
}

std::filesystem::path tempPath(const std::string& name)
{
    auto dir = std::filesystem::temp_directory_path();
    return dir / ("galay_rpc_" + name + "_" + std::to_string(::getpid()) + ".toml");
}

void writeFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream out(path);
    out << content;
}

} // namespace

int main()
{
    RpcConfig defaults;
    if (auto rc = expect(defaults.server.host == "0.0.0.0", "server host default changed")) {
        return rc;
    }
    if (auto rc = expect(defaults.server.port == 50051, "server port default changed")) {
        return rc;
    }
    if (auto rc = expect(defaults.client.default_timeout == std::chrono::milliseconds(3000),
                         "client timeout default changed")) {
        return rc;
    }
    if (auto rc = expect(defaults.pool.max_connections_per_endpoint == 4,
                         "pool max connections default changed")) {
        return rc;
    }
    if (auto rc = expect(defaults.retry.max_attempts == 1, "retry max attempts default changed")) {
        return rc;
    }
    if (auto rc = expect(defaults.discovery.kind == RpcDiscoveryKind::Local,
                         "discovery kind default changed")) {
        return rc;
    }

    auto missing = LoadRpcConfig(tempPath("missing"));
    if (auto rc = expect(!missing.has_value() &&
                             missing.error().code == RpcConfigErrorCode::MissingFile,
                         "missing config did not return MissingFile")) {
        return rc;
    }

    auto malformed_path = tempPath("malformed");
    writeFile(malformed_path, "[server\nport = 12\n");
    auto malformed = LoadRpcConfig(malformed_path);
    std::filesystem::remove(malformed_path);
    if (auto rc = expect(!malformed.has_value() &&
                             malformed.error().code == RpcConfigErrorCode::Malformed,
                         "malformed config did not return Malformed")) {
        return rc;
    }

    auto invalid_path = tempPath("invalid");
    writeFile(invalid_path,
              "[server]\n"
              "port = 0\n");
    auto invalid = LoadRpcConfig(invalid_path);
    std::filesystem::remove(invalid_path);
    if (auto rc = expect(!invalid.has_value() &&
                             invalid.error().code == RpcConfigErrorCode::InvalidValue,
                         "invalid port did not return InvalidValue")) {
        return rc;
    }

    auto partial_path = tempPath("partial");
    writeFile(partial_path,
              "[server]\n"
              "host = \"127.0.0.1\"\n"
              "port = 7000\n"
              "\n"
              "[client]\n"
              "default_timeout_ms = 250\n"
              "\n"
              "[pool]\n"
              "max_connections_per_endpoint = 8\n"
              "\n"
              "[retry]\n"
              "max_attempts = 3\n"
              "require_idempotent = false\n"
              "\n"
              "[governance.rate_limit]\n"
              "enabled = true\n"
              "capacity = 10\n"
              "\n"
              "[discovery]\n"
              "kind = \"etcd\"\n"
              "prefix = \"/galay/rpc\"\n"
              "\n"
              "[stream]\n"
              "max_frame_bytes = 4096\n"
              "\n"
              "[benchmark]\n"
              "requests = 1000\n");
    auto partial = LoadRpcConfig(partial_path);
    std::filesystem::remove(partial_path);
    if (auto rc = expect(partial.has_value(), "partial config did not load")) {
        return rc;
    }
    if (auto rc = expect(partial->server.host == "127.0.0.1" && partial->server.port == 7000,
                         "server partial overrides not applied")) {
        return rc;
    }
    if (auto rc = expect(partial->client.default_timeout == std::chrono::milliseconds(250),
                         "client timeout override not applied")) {
        return rc;
    }
    if (auto rc = expect(partial->pool.max_connections_per_endpoint == 8 &&
                             partial->pool.min_connections_per_endpoint ==
                                 defaults.pool.min_connections_per_endpoint,
                         "pool partial merge did not preserve defaults")) {
        return rc;
    }
    if (auto rc = expect(partial->retry.max_attempts == 3 && !partial->retry.require_idempotent,
                         "retry overrides not applied")) {
        return rc;
    }
    if (auto rc = expect(partial->governance.rate_limit.enabled &&
                             partial->governance.rate_limit.capacity == 10,
                         "governance rate limit overrides not applied")) {
        return rc;
    }
    if (auto rc = expect(partial->discovery.kind == RpcDiscoveryKind::Etcd &&
                             partial->discovery.prefix == "/galay/rpc",
                         "discovery overrides not applied")) {
        return rc;
    }
    if (auto rc = expect(partial->stream.max_frame_bytes == 4096,
                         "stream override not applied")) {
        return rc;
    }
    if (auto rc = expect(partial->benchmark.requests == 1000,
                         "benchmark override not applied")) {
        return rc;
    }

    std::cout << "RPC config PASS\n";
    return 0;
}
