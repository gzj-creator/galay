#include <galay/cpp/galay-rpc/kernel/rpc_config.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

using namespace galay::rpc;

namespace {

struct Config {
    int iterations = 1000000;
};

Config parseArgs(int argc, char** argv)
{
    Config config;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::string(argv[i]) == "-n") {
            config.iterations = std::max(1, std::stoi(argv[i + 1]));
        }
    }
    return config;
}

} // namespace

int main(int argc, char** argv)
{
    const Config bench = parseArgs(argc, argv);

    InMemoryRpcConfigProvider provider;
    RpcRuntimeConfig config = *provider.snapshot();
    RpcRoutePolicy policy;
    policy.timeout_ms = 123;
    policy.max_outstanding_requests = 8;
    config.route_policies.emplace(
        RpcRouteKey{"BenchService", "Unary", RpcCallMode::UNARY},
        policy);
    auto published = provider.publish(config);
    if (!published.has_value()) {
        return 1;
    }

    std::uint64_t read_guard = 0;
    const auto read_begin = std::chrono::steady_clock::now();
    for (int i = 0; i < bench.iterations; ++i) {
        const auto snapshot = provider.snapshot();
        if (snapshot == nullptr) {
            return 1;
        }
        read_guard += snapshot->version;
    }
    const auto read_end = std::chrono::steady_clock::now();

    std::uint64_t lookup_guard = 0;
    const auto lookup_begin = std::chrono::steady_clock::now();
    for (int i = 0; i < bench.iterations; ++i) {
        const auto snapshot = provider.snapshot();
        const auto& route_policy = findRpcRoutePolicy(
            *snapshot, "BenchService", "Unary", RpcCallMode::UNARY);
        lookup_guard += static_cast<std::uint64_t>(route_policy.timeout_ms);
    }
    const auto lookup_end = std::chrono::steady_clock::now();

    const double read_seconds = std::max(
        0.000001, std::chrono::duration<double>(read_end - read_begin).count());
    const double lookup_seconds = std::max(
        0.000001, std::chrono::duration<double>(lookup_end - lookup_begin).count());
    const double read_per_s = bench.iterations / read_seconds;
    const double lookup_per_s = bench.iterations / lookup_seconds;
    const double read_ns = (read_seconds * 1000000000.0) / bench.iterations;
    const double lookup_ns = (lookup_seconds * 1000000000.0) / bench.iterations;

    std::cout << "rpc config snapshot overhead"
              << " iterations=" << bench.iterations
              << " read_per_s=" << std::fixed << std::setprecision(2) << read_per_s
              << " lookup_per_s=" << lookup_per_s
              << " read_ns=" << read_ns
              << " lookup_ns=" << lookup_ns
              << " guard=" << (read_guard + lookup_guard)
              << "\n";

    return 0;
}
