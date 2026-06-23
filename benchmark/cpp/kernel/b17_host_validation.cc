/**
 * @file b17_host_validation.cc
 * @brief 衡量 Host 校验和 bind 参数错误快路径成本。
 */

#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/async/udp_socket.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

volatile std::size_t g_sink = 0;

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

    std::cout << std::left << std::setw(36) << name
              << std::right << std::setw(12) << std::fixed << std::setprecision(2) << nsPerOp
              << std::setw(16) << std::fixed << std::setprecision(2) << opsPerSec
              << "  checksum=" << checksum << '\n';
}

std::size_t requireParamInvalid(const std::expected<void, galay::kernel::IOError>& result)
{
    if (result.has_value() ||
        !galay::kernel::IOError::contains(result.error().code(), galay::kernel::kParamInvalid)) {
        throw std::runtime_error("expected kParamInvalid");
    }
    return 1;
}

} // namespace

int main()
{
    constexpr std::size_t iterations = 100'000;

    std::cout << "Kernel Host validation benchmark\n";
    std::cout << std::left << std::setw(36) << "Scenario"
              << std::right << std::setw(12) << "ns/op"
              << std::setw(16) << "ops/s" << '\n';

    measure("invalid Host construction", iterations, [](std::size_t i) {
        galay::kernel::Host host(galay::kernel::IPType::IPV4, "not-an-ip", 0);
        return static_cast<std::size_t>(!host.valid()) + (i & 1U);
    });

    measure("tcp invalid bind fast path", iterations, [](std::size_t i) {
        galay::async::TcpSocket socket(GHandle::invalid());
        galay::kernel::Host host(galay::kernel::IPType::IPV4, "not-an-ip", 0);
        return requireParamInvalid(socket.bind(host)) + (i & 1U);
    });

    measure("udp invalid bind fast path", iterations, [](std::size_t i) {
        galay::async::UdpSocket socket(GHandle::invalid());
        galay::kernel::Host host(galay::kernel::IPType::IPV4, "not-an-ip", 0);
        return requireParamInvalid(socket.bind(host)) + (i & 1U);
    });

    return static_cast<int>(g_sink == static_cast<std::size_t>(-1));
}
