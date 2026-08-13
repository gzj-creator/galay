/**
 * @file b7_v2_protocol_throughput.cc
 * @brief MCP 2026-07-28 request/response encode and parse throughput.
 */

#include <galay/cpp/galay-mcp/v2/common/protocol.h>

#include <charconv>
#include <chrono>
#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
    std::size_t iterations = 200'000;
    if (argc > 1) {
        const std::string_view text(argv[1]);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), iterations);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || iterations == 0) {
            std::cerr << "usage: benchmark_mcp_v2_protocol_throughput [positive-iterations]\n";
            return 2;
        }
    }

    galay::mcp::v2::RequestMeta meta;
    meta.clientInfo = galay::mcp::v2::Implementation{.name = "benchmark", .version = "1"};
    galay::mcp::v2::JsonRpcRequest request;
    request.id = int64_t{7};
    request.method = galay::mcp::v2::Methods::TOOLS_LIST;
    request.params = galay::mcp::v2::makeRequestParams(meta);

    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < 1'000; ++i) {
        auto parsed = galay::mcp::v2::parseRequest(request.toJson());
        if (!parsed) return 1;
        checksum += parsed->request.method.size();
    }
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto wire = request.toJson();
        auto parsed = galay::mcp::v2::parseRequest(wire);
        if (!parsed) return 1;
        checksum += parsed->request.method.size();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (elapsed <= 0 || checksum == 0) return 1;
    std::cout << "MCP v2 protocol iterations: " << iterations << '\n'
              << "Throughput: " << (static_cast<double>(iterations) * 1'000'000'000.0 / elapsed)
              << " request/s\n"
              << "Average: " << (static_cast<double>(elapsed) / iterations) << " ns/request\n"
              << "Checksum: " << checksum << '\n';
    return 0;
}
