/**
 * @file b16_resource_error_boundaries.cc
 * @brief 压测资源 RAII 关闭与 iovec 参数错误快路径。
 */

#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/async/udp_socket.h>

#if defined(USE_KQUEUE) || defined(USE_IOURING)
#include <galay/cpp/galay-kernel/async/async_file.h>
#endif

#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {

volatile std::size_t g_sink = 0;

using SequenceBoundaryResult = std::expected<int, galay::kernel::IOError>;

struct SequenceBoundaryFlow {
    void onLocal(galay::kernel::SequenceOps<SequenceBoundaryResult, 1>&) {}
};

using SequenceBoundaryStep = galay::kernel::LocalSequenceStep<
    SequenceBoundaryResult,
    1,
    SequenceBoundaryFlow,
    &SequenceBoundaryFlow::onLocal>;

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

#if defined(USE_KQUEUE) || defined(USE_IOURING)
std::filesystem::path makeTempFile()
{
    auto path = std::filesystem::temp_directory_path() / "galay_b16_async_file_raii.tmp";
    std::ofstream out(path);
    out << "galay";
    return path;
}
#endif

} // namespace

int main()
{
    constexpr std::size_t iterations = 20'000;

    std::cout << "Kernel resource/error boundary benchmark\n";
    std::cout << std::left << std::setw(36) << "Scenario"
              << std::right << std::setw(12) << "ns/op"
              << std::setw(16) << "ops/s" << '\n';

    measure("udp create+destruct", iterations, [](std::size_t i) {
        galay::async::UdpSocket socket;
        return static_cast<std::size_t>(socket.handle().fd >= 0) + (i & 1U);
    });

    measure("invalid readv/writev count", iterations, [](std::size_t i) {
        galay::async::TcpSocket socket(GHandle::invalid());
        std::array<struct iovec, 1> iovecs{};
        auto readv = socket.readv(iovecs, 2);
        auto writev = socket.writev(iovecs, 2);
        return static_cast<std::size_t>(readv.await_ready()) +
               static_cast<std::size_t>(writev.await_ready()) + (i & 1U);
    });

    measure("sequence overflow error", iterations, [](std::size_t i) {
        galay::kernel::IOController controller(GHandle::invalid());
        SequenceBoundaryFlow flow;
        SequenceBoundaryStep first(&flow);
        SequenceBoundaryStep second(&flow);
        galay::kernel::SequenceAwaitable<SequenceBoundaryResult, 1> sequence(&controller);

        sequence.queue(first);
        sequence.queue(second);
        if (!sequence.await_ready()) {
            throw std::runtime_error("overflowed sequence did not become ready");
        }
        auto result = sequence.await_resume();
        if (result.has_value() ||
            !galay::kernel::IOError::contains(result.error().code(), galay::kernel::kParamInvalid)) {
            throw std::runtime_error("overflowed sequence did not return kParamInvalid");
        }
        return static_cast<std::size_t>(1) + (i & 1U);
    });

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    const auto path = makeTempFile();
    measure("async_file open+destruct", iterations, [&](std::size_t i) {
        galay::async::AsyncFile file;
        auto opened = file.open(path.string(), galay::async::FileOpenMode::Read);
        return static_cast<std::size_t>(opened.has_value()) + (i & 1U);
    });
    std::filesystem::remove(path);
#endif

    return static_cast<int>(g_sink == static_cast<std::size_t>(-1));
}
