/**
 * @file b5_engine_bio_boundaries.cc
 * @brief SslEngine Memory BIO 显式错误返回路径压力基准。
 */

#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <galay/cpp/galay-ssl/ssl/ssl_engine.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

using namespace galay::ssl;

namespace {

bool parse_size(const char* text, size_t min_value, size_t* value)
{
    size_t parsed = 0;
    const char* end = text + std::char_traits<char>::length(text);
    const auto result = std::from_chars(text, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed < min_value) {
        return false;
    }
    *value = parsed;
    return true;
}

void print_usage(const char* program)
{
    std::cerr << "Usage: " << program << " [iterations>=1]\n";
}

} // namespace

int main(int argc, char* argv[])
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(argv[0]);
        return 0;
    }

    size_t iterations = 1000000;
    if (argc >= 2 && !parse_size(argv[1], 1, &iterations)) {
        print_usage(argv[0]);
        return 1;
    }

    SslContext ctx(SslMethod::TLS_Client);
    if (!ctx.is_valid()) {
        std::cerr << "ssl context invalid\n";
        return 1;
    }

    SslEngine engine(&ctx);
    if (!engine.is_valid()) {
        std::cerr << "ssl engine invalid\n";
        return 1;
    }

    const auto init = engine.init_memory_bio();
    if (!init) {
        std::cerr << "initMemoryBIO failed: " << init.error().message() << "\n";
        return 1;
    }

    constexpr std::string_view payload = "bio-boundary-payload";
    uint64_t bytes_moved = 0;
    uint64_t failures = 0;
    uint64_t rejected = 0;

    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        const auto fed = engine.feed_encrypted_input(payload.data(), payload.size());
        if (!fed) {
            ++failures;
            continue;
        }
        bytes_moved += *fed;

        const auto oversized = engine.feed_encrypted_input(
            payload.data(),
            static_cast<size_t>(std::numeric_limits<int>::max()) + 1U);
        if (oversized || oversized.error().code() != SslErrorCode::kBufferTooLarge) {
            ++failures;
            continue;
        }
        ++rejected;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const auto ns_per_op = iterations == 0 ? 0 : elapsed_ns / static_cast<int64_t>(iterations);

    std::cout << "ssl_engine_bio_boundaries"
              << " iterations=" << iterations
              << " bytes_moved=" << bytes_moved
              << " rejected=" << rejected
              << " failures=" << failures
              << " elapsed_ns=" << elapsed_ns
              << " ns_per_op=" << ns_per_op
              << std::endl;

    return failures == 0 ? 0 : 1;
}
