#include <galay/cpp/galay-utils/cache/bytes.hpp>
#include <galay/cpp/galay-utils/encoding/base64.hpp>
#include <galay/cpp/galay-utils/tool/pool.hpp>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

volatile std::size_t g_sink = 0;

struct Resettable {
    std::size_t value = 0;
    void reset() { value = 0; }
};

template <typename Fn>
void measure(const std::string& name, std::size_t iterations, Fn&& fn)
{
    std::size_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        checksum += fn(i);
    }
    const auto end = std::chrono::steady_clock::now();
    g_sink = checksum;

    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double nsPerOp = static_cast<double>(elapsedNs) / static_cast<double>(iterations);
    const double opsPerSec = 1'000'000'000.0 / nsPerOp;

    std::cout << std::left << std::setw(34) << name
              << std::right << std::setw(12) << std::fixed << std::setprecision(2) << nsPerOp
              << std::setw(16) << std::fixed << std::setprecision(2) << opsPerSec
              << "  checksum=" << checksum << '\n';
}

} // namespace

int main()
{
    constexpr std::size_t iterations = 200'000;

    std::cout << "Utils resource/error boundary benchmark\n";
    std::cout << std::left << std::setw(34) << "Scenario"
              << std::right << std::setw(12) << "ns/op"
              << std::setw(16) << "ops/s" << '\n';

    measure("object_pool late lease", iterations, [](std::size_t i) {
        galay::utils::ObjectPool<Resettable>::Ptr lease;
        {
            galay::utils::ObjectPool<Resettable> pool(0, 1);
            lease = pool.acquire();
            lease->value = i;
        }
        const auto value = lease->value;
        lease.reset();
        return value & 0xFFU;
    });

    measure("base64 whitespace decode", iterations, [](std::size_t i) {
        const auto decoded = galay::utils::Base64Util::Base64Decode("SGVs\r\n bG8=\t", true);
        return decoded.size() + (i & 1U);
    });

    measure("bytes owned c_str", iterations, [](std::size_t i) {
        const char raw[] = {'a', 'b', 'c'};
        galay::utils::Bytes bytes(raw, sizeof(raw));
        const char* text = bytes.c_str();
        return static_cast<std::size_t>(text[3] == '\0') + (i & 1U);
    });

    return static_cast<int>(g_sink == static_cast<std::size_t>(-1));
}
