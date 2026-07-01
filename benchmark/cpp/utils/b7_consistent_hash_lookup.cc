#include <galay/cpp/galay-utils/algorithm/consistent_hash.hpp>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

volatile std::size_t g_sink = 0;

} // namespace

int main()
{
    constexpr std::size_t iterations = 200'000;

    galay::utils::ConsistentHash hash(64);
    hash.addNode({"node-a", "127.0.0.1:9001", 1});
    hash.addNode({"node-b", "127.0.0.1:9002", 1});
    hash.addNode({"node-c", "127.0.0.1:9003", 2});

    std::size_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        auto node = hash.getNode("request-" + std::to_string(i));
        if (!node.has_value()) {
            std::cerr << "consistent hash lookup returned no node\n";
            return 1;
        }
        checksum += node->id.size();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    g_sink = checksum;

    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const double ns_per_op = static_cast<double>(elapsed_ns) / static_cast<double>(iterations);
    const double ops_per_sec = 1'000'000'000.0 / ns_per_op;

    std::cout << "consistent_hash_lookup"
              << " iterations=" << iterations
              << " ns/op=" << std::fixed << std::setprecision(2) << ns_per_op
              << " ops/s=" << std::fixed << std::setprecision(2) << ops_per_sec
              << " checksum=" << checksum << '\n';
    return static_cast<int>(g_sink == static_cast<std::size_t>(-1));
}
