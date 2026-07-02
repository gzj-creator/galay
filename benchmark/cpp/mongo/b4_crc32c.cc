#include <galay/cpp/galay-mongo/protoc/crc32c.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

std::expected<size_t, std::string> parsePositiveSize(std::string_view text,
                                                     size_t fallback)
{
    if (text.empty()) {
        return fallback;
    }

    size_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || value == 0) {
        return std::unexpected("expected a positive integer");
    }
    return value;
}

std::string makePayload(size_t bytes)
{
    std::string payload;
    payload.resize(bytes);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 131u + 17u) & 0xFFu);
    }
    return payload;
}

} // namespace

int main(int argc, char** argv)
{
    size_t iterations = 200000;
    if (argc > 1) {
        auto parsed = parsePositiveSize(argv[1], iterations);
        if (!parsed) {
            std::cerr << "invalid iterations: " << parsed.error() << '\n';
            return 1;
        }
        iterations = *parsed;
    }

    size_t payload_bytes = 1024;
    if (argc > 2) {
        auto parsed = parsePositiveSize(argv[2], payload_bytes);
        if (!parsed) {
            std::cerr << "invalid payload bytes: " << parsed.error() << '\n';
            return 1;
        }
        payload_bytes = *parsed;
    }

    const std::string payload = makePayload(payload_bytes);
    uint32_t checksum_accumulator = 2166136261u;

    constexpr size_t kWarmupIterations = 4096;
    for (size_t i = 0; i < kWarmupIterations; ++i) {
        const uint32_t checksum = galay::mongo::protocol::detail::crc32c(payload.data(),
                                                                         payload.size());
        checksum_accumulator = (checksum_accumulator * 16777619u) ^ checksum;
    }

    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        const uint32_t checksum = galay::mongo::protocol::detail::crc32c(payload.data(),
                                                                         payload.size());
        checksum_accumulator = (checksum_accumulator * 16777619u) ^ checksum;
    }
    const auto finish = std::chrono::steady_clock::now();

    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
    const double ns_per_op = static_cast<double>(elapsed_ns) / static_cast<double>(iterations);
    const double mib_processed =
        (static_cast<double>(payload_bytes) * static_cast<double>(iterations)) /
        (1024.0 * 1024.0);
    const double seconds = static_cast<double>(elapsed_ns) / 1'000'000'000.0;
    const double mib_per_second = seconds > 0.0 ? mib_processed / seconds : 0.0;

    std::cout << "B4-MongoCRC32C iterations=" << iterations
              << " payload_bytes=" << payload_bytes << '\n';
    std::cout << "crc32c_ns_per_op=" << ns_per_op
              << " mib_per_second=" << mib_per_second
              << " checksum_accumulator=0x" << std::hex << checksum_accumulator
              << std::dec << '\n';
    return 0;
}
