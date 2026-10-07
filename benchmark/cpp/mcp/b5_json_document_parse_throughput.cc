/**
 * @file b5_json_document_parse_throughput.cc
 * @brief MCP JsonDocument 解析吞吐基准。
 * @details serde 的 json::parse 每次解析都会分配新的解析 State，
 *          本基准只测量解析吞吐，不再统计解析器对象分配。
 */

#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-mcp/common/mcp_json.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

bool fail(std::string_view message)
{
    std::cerr << message << '\n';
    return false;
}

bool parse_iterations(int argc, char** argv, std::size_t& iterations)
{
    if (argc <= 1) {
        return true;
    }
    std::string_view text(argv[1]);
    std::size_t parsed = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed == 0) {
        return fail("usage: benchmark_mcp_json_document_parse_throughput [positive-iterations]");
    }
    iterations = parsed;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    std::size_t iterations = 200'000;
    if (!parse_iterations(argc, argv, iterations)) {
        return 2;
    }

    constexpr std::string_view json =
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"}}})";
    constexpr std::size_t warmup_iterations = 1'000;
    std::uint64_t checksum = 0;

    for (std::size_t i = 0; i < warmup_iterations; ++i) {
        auto doc = galay::mcp::JsonDocument::parse(json);
        if (!doc) {
            std::cerr << "warmup parse failed: " << doc.error().to_string() << '\n';
            return 1;
        }
        auto id = doc->root().at("id").as_uint64();
        if (!id) {
            std::cerr << "warmup id read failed: " << id.error() << '\n';
            return 1;
        }
        checksum += *id;
    }

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        auto doc = galay::mcp::JsonDocument::parse(json);
        if (!doc) {
            std::cerr << "parse failed: " << doc.error().to_string() << '\n';
            return 1;
        }
        auto id = doc->root().at("id").as_uint64();
        if (!id) {
            std::cerr << "id read failed: " << id.error() << '\n';
            return 1;
        }
        checksum += *id;
    }
    const auto end = std::chrono::steady_clock::now();

    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    if (elapsed_ns <= 0 || checksum == 0) {
        std::cerr << "invalid benchmark result\n";
        return 1;
    }

    const double seconds = static_cast<double>(elapsed_ns) / 1'000'000'000.0;
    const double parses_per_second = static_cast<double>(iterations) / seconds;
    const double ns_per_parse = static_cast<double>(elapsed_ns) / static_cast<double>(iterations);

    std::cout << "MCP JsonDocument parse iterations: " << iterations << '\n';
    std::cout << "Elapsed: " << elapsed_ns / 1000 << " us\n";
    std::cout << "Throughput: " << parses_per_second << " parses/s\n";
    std::cout << "Average: " << ns_per_parse << " ns/parse\n";
    std::cout << "Checksum: " << checksum << '\n';
    return 0;
}
