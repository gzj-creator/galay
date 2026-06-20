#include <galay/cpp/galay-utils/cache/ring_buffer.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

volatile std::size_t g_sink = 0;

struct Result {
    std::string name;
    double nsPerOp;
    double mbPerSec;
    std::size_t checksum;
};

template<typename Fn>
Result measure(std::string name, std::size_t iterations, std::size_t bytesPerIteration, Fn&& fn) {
    std::size_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        checksum += fn(i);
    }
    const auto end = std::chrono::steady_clock::now();

    g_sink = checksum;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double nsPerOp = static_cast<double>(ns) / static_cast<double>(iterations);
    const double totalMb = static_cast<double>(iterations * bytesPerIteration) / (1024.0 * 1024.0);
    const double seconds = static_cast<double>(ns) / 1'000'000'000.0;
    return Result{std::move(name), nsPerOp, totalMb / seconds, checksum};
}

void printResult(const Result& result) {
    std::cout << std::left << std::setw(28) << result.name
              << std::right << std::setw(12) << std::fixed << std::setprecision(2) << result.nsPerOp
              << std::setw(14) << std::fixed << std::setprecision(2) << result.mbPerSec
              << "  checksum=" << result.checksum << '\n';
}

} // namespace

int main() {
    constexpr std::size_t capacity = 64 * 1024;
    constexpr std::size_t chunk = 1024;
    constexpr std::size_t iterations = 5'000'000;

    std::vector<std::byte> writeData(chunk, std::byte{'x'});
    std::vector<std::byte> readData(chunk);

    std::cout << "RingBuffer benchmark\n";
    std::cout << "Build with -O3 -DNDEBUG. Capacity=" << capacity
              << ", chunk=" << chunk << ", iterations=" << iterations << '\n';
    std::cout << std::left << std::setw(28) << "Scenario"
              << std::right << std::setw(12) << "ns/op"
              << std::setw(14) << "MB/s" << '\n';

    {
        galay::utils::RingBuffer buffer(capacity);
        auto result = measure("write+consume", iterations, chunk, [&](std::size_t i) {
            const auto written = buffer.write(writeData.data(), writeData.size());
            buffer.consume(written);
            return written + i % 17;
        });
        printResult(result);
    }

    {
        galay::utils::RingBuffer buffer(capacity);
        auto result = measure("write+read", iterations, chunk, [&](std::size_t i) {
            const auto written = buffer.write(writeData.data(), writeData.size());
            const auto read = buffer.read(readData.data(), readData.size());
            return written + read + i % 17;
        });
        printResult(result);
    }

    {
        galay::utils::RingBuffer buffer(4096);
        auto result = measure("wrap-around", iterations, 256, [&](std::size_t i) {
            const auto written = buffer.write(writeData.data(), 256);
            buffer.consume(128);
            if (buffer.full()) {
                buffer.consume(buffer.readable() / 2);
            }
            return written + buffer.readable() + i % 17;
        });
        printResult(result);
    }

    return static_cast<int>(g_sink == static_cast<std::size_t>(-1));
}
