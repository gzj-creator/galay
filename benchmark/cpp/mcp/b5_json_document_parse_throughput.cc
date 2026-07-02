/**
 * @file b5_json_document_parse_throughput.cc
 * @brief MCP JsonDocument 解析吞吐与解析器分配基准。
 */

#include <galay/cpp/galay-mcp/common/mcp_json.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string_view>

namespace {

std::atomic<bool> g_count_parser_allocations{false};
std::atomic<std::size_t> g_parser_allocations{0};

bool fail(std::string_view message)
{
    std::cerr << message << '\n';
    return false;
}

bool parseIterations(int argc, char** argv, std::size_t& iterations)
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

std::size_t takeParserAllocationCount()
{
    return g_parser_allocations.exchange(0, std::memory_order_relaxed);
}

} // namespace

void* operator new(std::size_t size)
{
    if (g_count_parser_allocations.load(std::memory_order_relaxed) &&
        size == sizeof(simdjson::dom::parser)) {
        g_parser_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* ptr = std::malloc(size)) {
        return ptr;
    }
    throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

int main(int argc, char** argv)
{
    std::size_t iterations = 200'000;
    if (!parseIterations(argc, argv, iterations)) {
        return 2;
    }

    constexpr std::string_view json =
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"}}})";
    constexpr std::size_t warmup_iterations = 1'000;
    std::uint64_t checksum = 0;

    for (std::size_t i = 0; i < warmup_iterations; ++i) {
        auto doc = galay::mcp::JsonDocument::parse(json);
        if (!doc) {
            std::cerr << "warmup parse failed: " << doc.error().toString() << '\n';
            return 1;
        }
        auto id = doc->root()["id"].get_uint64();
        if (id.error()) {
            std::cerr << "warmup id read failed: " << simdjson::error_message(id.error()) << '\n';
            return 1;
        }
        checksum += id.value();
    }

    takeParserAllocationCount();
    g_count_parser_allocations.store(true, std::memory_order_relaxed);
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        auto doc = galay::mcp::JsonDocument::parse(json);
        if (!doc) {
            g_count_parser_allocations.store(false, std::memory_order_relaxed);
            std::cerr << "parse failed: " << doc.error().toString() << '\n';
            return 1;
        }
        auto id = doc->root()["id"].get_uint64();
        if (id.error()) {
            g_count_parser_allocations.store(false, std::memory_order_relaxed);
            std::cerr << "id read failed: " << simdjson::error_message(id.error()) << '\n';
            return 1;
        }
        checksum += id.value();
    }
    const auto end = std::chrono::steady_clock::now();
    g_count_parser_allocations.store(false, std::memory_order_relaxed);

    const auto parser_allocations = takeParserAllocationCount();
    if (parser_allocations != 0) {
        std::cerr << "parser object allocations during measured parse loop: " << parser_allocations << '\n';
        return 1;
    }

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
    std::cout << "Parser object allocations: " << parser_allocations << '\n';
    std::cout << "Checksum: " << checksum << '\n';
    return 0;
}
