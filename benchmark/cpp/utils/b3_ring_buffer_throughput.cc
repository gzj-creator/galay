#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-utils/buffer/ring_buffer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/uio.h>
#endif

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

void print_result(const Result& result) {
    std::cout << std::left << std::setw(28) << result.name
              << std::right << std::setw(12) << std::fixed << std::setprecision(2) << result.nsPerOp
              << std::setw(14) << std::fixed << std::setprecision(2) << result.mbPerSec
              << "  checksum=" << result.checksum << '\n';
}

#if defined(__unix__) || defined(__APPLE__)
void bench_wrapped_iovec(std::size_t capacity, std::size_t iterations) {
    galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> buffer(capacity);
    const std::size_t actualCapacity = buffer.capacity();
    std::vector<std::byte> prefix(actualCapacity - 64, std::byte{'a'});
    std::vector<std::byte> wrapped(256, std::byte{'b'});

    const auto prefixWritten = buffer.try_write_batch(prefix.data(), prefix.size());
    buffer.consume(actualCapacity - 128);
    const auto wrappedWritten =
        buffer.try_write_batch(wrapped.data(), wrapped.size());
    if (prefixWritten != prefix.size() || wrappedWritten != wrapped.size()) {
        std::cerr << "failed to prepare wrapped iovec benchmark\n";
        return;
    }

    std::array<struct iovec, 2> iovecs{};
    auto result = measure("wrap-iovec-" + std::to_string(capacity / 1024) + "KB",
                          iterations, buffer.readable(), [&](std::size_t i) {
        const auto count = buffer.get_read_iovecs(iovecs);
        return count + iovecs[0].iov_len + i % 17;
    });
    print_result(result);
}
#endif

} // namespace

int main() {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    constexpr std::size_t capacity = 64 * 1024;
    constexpr std::size_t chunk = 1024;
    constexpr std::size_t iterations = 5'000'000;

    std::vector<std::byte> writeData(chunk, std::byte{'x'});
    std::vector<std::byte> read_data(chunk);

    std::cout << "RingBuffer benchmark\n";
    std::cout << "Build with -O3 -DNDEBUG. Capacity=" << capacity
              << ", chunk=" << chunk << ", iterations=" << iterations << '\n';
    std::cout << std::left << std::setw(28) << "Scenario"
              << std::right << std::setw(12) << "ns/op"
              << std::setw(14) << "MB/s" << '\n';

    {
        galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> buffer(capacity);
        auto result = measure("write+consume", iterations, chunk, [&](std::size_t i) {
            const auto written =
                buffer.try_write_batch(writeData.data(), writeData.size());
            buffer.consume(written);
            return written + i % 17;
        });
        print_result(result);
    }

    {
        galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> buffer(capacity);
        auto result = measure("write+read", iterations, chunk, [&](std::size_t i) {
            const auto written =
                buffer.try_write_batch(writeData.data(), writeData.size());
            const auto read =
                buffer.try_read_batch(read_data.data(), read_data.size());
            return written + read + i % 17;
        });
        print_result(result);
    }

    {
        galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> buffer(4096);
        auto result = measure("wrap-around", iterations, 256, [&](std::size_t i) {
            const auto written = buffer.try_write_batch(writeData.data(), 256);
            buffer.consume(128);
            if (buffer.full()) {
                buffer.consume(buffer.readable() / 2);
            }
            return written + buffer.readable() + i % 17;
        });
        print_result(result);
    }

#if defined(__unix__) || defined(__APPLE__)
    bench_wrapped_iovec(4096, iterations);
    bench_wrapped_iovec(64 * 1024, iterations);
    bench_wrapped_iovec(128 * 1024, iterations);
#endif

    return static_cast<int>(g_sink == static_cast<std::size_t>(-1));
}
