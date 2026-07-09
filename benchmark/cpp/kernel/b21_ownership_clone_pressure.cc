/**
 * @file b21_ownership_clone_pressure.cc
 * @brief 压测 kernel move-only ownership 表面的 clone/move 与 awaitable 构造成本。
 */

#include <galay/cpp/galay-kernel/async/aio_file.h>
#include <galay/cpp/galay-kernel/common/buffer.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/common/timer_manager.hpp>
#include <galay/cpp/galay-kernel/core/awaitable.h>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kDefaultIterations = 100000;
constexpr std::size_t kPayloadSize = 4096;
constexpr uint64_t kTickNs = 1'000'000ULL;

struct Sample {
    double elapsed_ms = 0.0;
    double ops_per_sec = 0.0;
};

Sample makeSample(std::size_t iterations, std::chrono::steady_clock::duration elapsed)
{
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return {
        .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
        .ops_per_sec = elapsed_ns > 0
            ? static_cast<double>(iterations) * 1'000'000'000.0 / static_cast<double>(elapsed_ns)
            : 0.0,
    };
}

bool parseIterations(int argc, char** argv, std::size_t& iterations)
{
    if (argc <= 1) {
        iterations = kDefaultIterations;
        return true;
    }

    char* end = nullptr;
    const auto parsed = std::strtoull(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || parsed == 0) {
        std::cerr << "[B21] iterations must be a positive integer\n";
        return false;
    }
    iterations = static_cast<std::size_t>(parsed);
    return true;
}

bool benchBufferClone(std::size_t iterations, const std::string& payload)
{
    std::size_t bytes = 0;
    unsigned char checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        Buffer source(payload);
        Buffer cloned = source.clone();
        if (cloned.data() == source.data() || cloned.length() != payload.size()) {
            std::cerr << "[B21] Buffer clone did not produce independent storage\n";
            return false;
        }
        source.data()[0] = static_cast<char>('A' + (i % 26));
        checksum ^= static_cast<unsigned char>(cloned.data()[0]);
        bytes += cloned.length();
    }
    const auto sample = makeSample(iterations, std::chrono::steady_clock::now() - start);
    std::cout << "OwnershipBufferClone iterations=" << iterations
              << " bytes=" << bytes
              << " checksum=" << static_cast<unsigned>(checksum)
              << " elapsed_ms=" << std::fixed << std::setprecision(3) << sample.elapsed_ms
              << " ops_per_sec=" << std::setprecision(0) << sample.ops_per_sec << "\n";
    return true;
}

bool benchBufferMove(std::size_t iterations, const std::string& payload)
{
    std::size_t bytes = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        Buffer source(payload);
        Buffer moved(std::move(source));
        if (source.data() != nullptr || source.length() != 0 || moved.length() != payload.size()) {
            std::cerr << "[B21] Buffer move did not transfer ownership cleanly\n";
            return false;
        }
        bytes += moved.length();
    }
    const auto sample = makeSample(iterations, std::chrono::steady_clock::now() - start);
    std::cout << "OwnershipBufferMove iterations=" << iterations
              << " bytes=" << bytes
              << " elapsed_ms=" << std::fixed << std::setprecision(3) << sample.elapsed_ms
              << " ops_per_sec=" << std::setprecision(0) << sample.ops_per_sec << "\n";
    return true;
}

bool benchTimerAwaitableConstruction(std::size_t iterations)
{
    std::size_t constructed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        TimingWheelTimerManager manager(kTickNs);
        TimingWheelTimerManager moved_manager(std::move(manager));
        if (moved_manager.during() != kTickNs) {
            std::cerr << "[B21] moved TimingWheelTimerManager lost tick duration\n";
            return false;
        }

        SleepAwaitable sleep_awaitable(1ms);
        SleepAwaitable moved_sleep(std::move(sleep_awaitable));
        if (!moved_sleep.m_timer) {
            std::cerr << "[B21] moved SleepAwaitable lost timer state\n";
            return false;
        }

        char buffer = 0;
        IOController controller(GHandle{.fd = -1});
        auto timed_recv = RecvAwaitable(&controller, &buffer, 1).timeout(1ms);
        if (timed_recv.m_inner.m_controller != &controller || !timed_recv.m_timer) {
            std::cerr << "[B21] WithTimeout<RecvAwaitable> construction lost state\n";
            return false;
        }

#ifdef USE_EPOLL
        std::vector<struct iocb*> pending;
        galay::async::AioCommitAwaitable aio_commit(nullptr, 0, -1, std::move(pending), 0);
        galay::async::AioCommitAwaitable moved_aio_commit(std::move(aio_commit));
        if (!moved_aio_commit.await_ready()) {
            std::cerr << "[B21] moved AioCommitAwaitable should remain ready with no pending ops\n";
            return false;
        }
#endif

#ifdef USE_IOURING
        ReadyRecvChunk chunk;
        chunk.owner = std::make_shared<int>(1);
        ReadyRecvChunk moved_chunk(std::move(chunk));
        if (!moved_chunk.owner || chunk.owner) {
            std::cerr << "[B21] ReadyRecvChunk move should transfer owner\n";
            return false;
        }
#endif

        ++constructed;
    }
    const auto sample = makeSample(constructed, std::chrono::steady_clock::now() - start);
    std::cout << "OwnershipTimerAwaitableConstruction iterations=" << constructed
              << " elapsed_ms=" << std::fixed << std::setprecision(3) << sample.elapsed_ms
              << " ops_per_sec=" << std::setprecision(0) << sample.ops_per_sec << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::size_t iterations = 0;
    if (!parseIterations(argc, argv, iterations)) {
        return 1;
    }

    std::string payload(kPayloadSize, 'x');
    payload[0] = 'b';

    bool ok = true;
    ok = benchBufferClone(iterations, payload) && ok;
    ok = benchBufferMove(iterations, payload) && ok;
    ok = benchTimerAwaitableConstruction(iterations) && ok;
    if (ok) {
        std::cout << "B21-OwnershipClonePressure PASS\n";
    }
    return ok ? 0 : 1;
}
