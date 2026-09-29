/** @file micro_measurement.h
 *  @brief Fixed-work microbenchmark protocol; calibration belongs to the driver.
 */
#pragma once
#include <charconv>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <iostream>
#include <string_view>

namespace galay::benchmark {
struct MicroOptions {
    std::size_t iterations = 20'000;
    std::string_view sample;
    bool diagnostics = false;
};
inline bool parseMicroOptions(int argc, char** argv, MicroOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--diagnostics") { options.diagnostics = true; continue; }
        if (++i == argc) { return false; }
        const std::string_view value(argv[i]);
        if (arg == "--sample") { options.sample = value; }
        else if (arg == "--iterations") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.iterations);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                options.iterations < 1 || options.iterations > 1'000'000'000) { return false; }
        } else { return false; }
    }
    return true;
}
struct MicroMeasurement {
    std::size_t iterations = 0;
    std::chrono::steady_clock::duration elapsed{};
    double cpu_ns = 0;
    std::size_t fallbackEstimate = 0;
};
// Two process CPU clock reads per segment, never per operation. Includes all
// participating threads, but excludes the separate warmup and process startup.
class MicroTimer {
public:
    MicroTimer() : m_cpu(std::clock()), m_wall(std::chrono::steady_clock::now()) {}
    void finish(MicroMeasurement& measured) const {
        measured.elapsed = std::chrono::steady_clock::now() - m_wall;
        const auto cpu = std::clock();
        measured.cpu_ns = m_cpu == std::clock_t(-1) || cpu == std::clock_t(-1)
            ? -1 : 1e9 * static_cast<double>(cpu - m_cpu) / CLOCKS_PER_SEC;
    }
private:
    std::clock_t m_cpu;
    std::chrono::steady_clock::time_point m_wall;
};
} // namespace galay::benchmark
