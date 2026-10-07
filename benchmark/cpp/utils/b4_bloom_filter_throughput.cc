#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-utils/algorithm/bloom_filter.hpp>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

volatile std::uint64_t g_sink = 0;

struct Result {
    std::string name;
    double nsPerOp;
    double mopsPerSec;
    std::uint64_t checksum;
};

uint64_t stable_hash(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

template<typename Fn>
Result measure(std::string name, std::size_t iterations, Fn&& fn) {
    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        checksum += fn(i);
    }
    const auto end = std::chrono::steady_clock::now();

    g_sink = checksum;
    const auto elapsedNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double nsPerOp = static_cast<double>(elapsedNs) / static_cast<double>(iterations);
    const double seconds = static_cast<double>(elapsedNs) / 1000000000.0;
    const double mopsPerSec = (static_cast<double>(iterations) / seconds) / 1000000.0;
    return Result{std::move(name), nsPerOp, mopsPerSec, checksum};
}

void print_result(const Result& result) {
    std::cout << std::left << std::setw(24) << result.name
              << std::right << std::setw(12) << std::fixed << std::setprecision(2)
              << result.nsPerOp
              << std::setw(14) << std::fixed << std::setprecision(2)
              << result.mopsPerSec
              << "  checksum=" << result.checksum << '\n';
}

} // namespace

int main() {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    constexpr std::size_t items = 1000000;
    constexpr double targetFalsePositiveRate = 0.01;

    std::vector<uint64_t> inserted;
    std::vector<uint64_t> missing;
    inserted.reserve(items);
    missing.reserve(items);

    for (uint64_t i = 0; i < items; ++i) {
        inserted.push_back(stable_hash(i));
        missing.push_back(stable_hash(i + 10000000ULL));
    }

    galay::utils::BloomFilter<uint64_t> filter =
        galay::utils::BloomFilter<uint64_t>::from_expected_items(
            items, targetFalsePositiveRate);

    std::cout << "BloomFilter benchmark\n";
    std::cout << "Build with -O3 -DNDEBUG. Items=" << items
              << ", target_fpp=" << targetFalsePositiveRate
              << ", bits=" << filter.bit_count()
              << ", blocks=" << filter.block_count() << '\n';
    std::cout << std::left << std::setw(24) << "Scenario"
              << std::right << std::setw(12) << "ns/op"
              << std::setw(14) << "Mops/s" << '\n';

    print_result(measure("addHash", items, [&](std::size_t i) {
        filter.add_hash(inserted[i]);
        return inserted[i] & 0xffu;
    }));

    print_result(measure("hit query", items, [&](std::size_t i) {
        return filter.possibly_contains_hash(inserted[i]) ? 1u : 0u;
    }));

    const auto missResult = measure("miss query", items, [&](std::size_t i) {
        return filter.possibly_contains_hash(missing[i]) ? 1u : 0u;
    });
    print_result(missResult);

    std::cout << "Observed false positives=" << missResult.checksum
              << " out of " << items << '\n';

    return static_cast<int>(g_sink == static_cast<std::uint64_t>(-1));
}
