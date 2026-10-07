#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-utils/buffer/byte_queue_view.hpp>

#include <chrono>
#include <cstdint>
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
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double nsPerOp = static_cast<double>(elapsedNs) / static_cast<double>(iterations);
    const double totalMb = static_cast<double>(iterations * bytesPerIteration) / (1024.0 * 1024.0);
    const double seconds = static_cast<double>(elapsedNs) / 1'000'000'000.0;
    return Result{std::move(name), nsPerOp, totalMb / seconds, checksum};
}

void print_result(const Result& result) {
    std::cout << std::left << std::setw(30) << result.name
              << std::right << std::setw(12) << std::fixed << std::setprecision(2) << result.nsPerOp
              << std::setw(14) << std::fixed << std::setprecision(2) << result.mbPerSec
              << "  checksum=" << result.checksum << '\n';
}

void write_big_endian32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFFu));
    out.push_back(static_cast<char>((value >> 16) & 0xFFu));
    out.push_back(static_cast<char>((value >> 8) & 0xFFu));
    out.push_back(static_cast<char>(value & 0xFFu));
}

std::uint32_t read_big_endian32(std::string_view view) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(view[0])) << 24u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(view[1])) << 16u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(view[2])) << 8u) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(view[3]));
}

} // namespace

int main() {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    constexpr std::size_t iterations = 5'000'000;
    constexpr std::size_t chunk = 256;

    std::string payload(chunk, 'x');
    std::string frame;
    write_big_endian32(frame, static_cast<std::uint32_t>(payload.size()));
    frame += payload;

    std::cout << "ByteQueueView benchmark\n";
    std::cout << "Build with -O3 -DNDEBUG. Chunk=" << chunk
              << ", iterations=" << iterations << '\n';
    std::cout << std::left << std::setw(30) << "Scenario"
              << std::right << std::setw(12) << "ns/op"
              << std::setw(14) << "MB/s" << '\n';

    {
        galay::utils::ByteQueueView queue(64 * 1024);
        auto result = measure("append+consume", iterations, chunk, [&](std::size_t i) {
            queue.append(payload);
            const auto size = queue.size();
            queue.consume(size);
            return size + i % 17;
        });
        print_result(result);
    }

    {
        galay::utils::ByteQueueView queue(64 * 1024);
        auto result = measure("incremental compact", iterations, chunk, [&](std::size_t i) {
            queue.append(payload);
            queue.consume(chunk / 2);
            if (queue.size() > 64 * 1024) {
                queue.consume(queue.size() - chunk);
            }
            return queue.size() + i % 17;
        });
        print_result(result);
    }

    {
        galay::utils::ByteQueueView queue(64 * 1024);
        auto result = measure("framed parse", iterations, frame.size(), [&](std::size_t i) {
            queue.append(frame);
            std::size_t parsed = 0;
            while (queue.has(4)) {
                const auto header = queue.view(0, 4);
                const auto length = static_cast<std::size_t>(read_big_endian32(header));
                if (!queue.has(4 + length)) {
                    break;
                }
                parsed += length;
                queue.consume(4 + length);
            }
            return parsed + i % 17;
        });
        print_result(result);
    }

    return static_cast<int>(g_sink == static_cast<std::size_t>(-1));
}
