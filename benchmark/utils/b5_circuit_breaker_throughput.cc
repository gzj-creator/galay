#include <galay/cpp/galay-utils/tool/circuit_breaker.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iomanip>
#include <iostream>
#include <thread>
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

using ExpectedInt = std::expected<int, galay::utils::CircuitBreakerError>;

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
    const double seconds = static_cast<double>(elapsedNs) / 1'000'000'000.0;
    const double mopsPerSec = (static_cast<double>(iterations) / seconds) / 1'000'000.0;
    return Result{std::move(name), nsPerOp, mopsPerSec, checksum};
}

void printResult(const Result& result) {
    std::cout << std::left << std::setw(28) << result.name
              << std::right << std::setw(12) << std::fixed << std::setprecision(2)
              << result.nsPerOp
              << std::setw(14) << std::fixed << std::setprecision(2)
              << result.mopsPerSec
              << "  checksum=" << result.checksum << '\n';
}

template<typename Fn>
Result measureConcurrent(std::string name,
                         std::size_t threadCount,
                         std::size_t iterationsPerThread,
                         Fn&& fn) {
    std::vector<std::thread> threads;
    std::vector<std::uint64_t> checksums(threadCount, 0);
    threads.reserve(threadCount);

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        threads.emplace_back([&, threadIndex]() {
            std::uint64_t localChecksum = 0;
            for (std::size_t i = 0; i < iterationsPerThread; ++i) {
                localChecksum += fn(threadIndex, i);
            }
            checksums[threadIndex] = localChecksum;
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    const auto end = std::chrono::steady_clock::now();

    std::uint64_t checksum = 0;
    for (auto value : checksums) {
        checksum += value;
    }

    g_sink = checksum;
    const auto elapsedNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const auto iterations = threadCount * iterationsPerThread;
    const double nsPerOp = static_cast<double>(elapsedNs) / static_cast<double>(iterations);
    const double seconds = static_cast<double>(elapsedNs) / 1'000'000'000.0;
    const double mopsPerSec = (static_cast<double>(iterations) / seconds) / 1'000'000.0;
    return Result{std::move(name), nsPerOp, mopsPerSec, checksum};
}

} // namespace

int main() {
    constexpr std::size_t iterations = 5'000'000;
    const std::size_t hardwareThreads = std::max(2u, std::thread::hardware_concurrency());
    const std::size_t threadCount = static_cast<std::size_t>(hardwareThreads);
    constexpr std::size_t iterationsPerThread = 500'000;

    std::cout << "CircuitBreaker benchmark\n";
    std::cout << "Build with -O3 -DNDEBUG. Iterations=" << iterations << '\n';
    std::cout << std::left << std::setw(28) << "Scenario"
              << std::right << std::setw(12) << "ns/op"
              << std::setw(14) << "Mops/s" << '\n';

    {
        galay::utils::CircuitBreakerConfig config;
        config.failureThreshold = iterations + 1;
        galay::utils::CircuitBreaker breaker(config);
        printResult(measure("closed execute success", iterations, [&](std::size_t i) {
            auto result = breaker.execute([&]() -> ExpectedInt {
                return static_cast<int>(i & 0xffu);
            });
            return static_cast<std::uint64_t>(*result);
        }));
    }

    {
        galay::utils::CircuitBreakerConfig config;
        config.failureThreshold = iterations + 1;
        galay::utils::CircuitBreaker breaker(config);
        printResult(measure("closed execute failure", iterations, [&](std::size_t) {
            auto result = breaker.execute([]() -> ExpectedInt {
                return std::unexpected(galay::utils::CircuitBreakerError::Open);
            });
            return result.has_value() ? 1u : 0u;
        }));
    }

    {
        galay::utils::CircuitBreakerConfig config;
        config.resetTimeout = std::chrono::hours(1);
        galay::utils::CircuitBreaker breaker(config);
        breaker.forceOpen();
        printResult(measure("open execute reject", iterations, [&](std::size_t) {
            auto result = breaker.execute([]() -> ExpectedInt {
                return 1;
            });
            return result.has_value() ? 1u : 0u;
        }));
    }

    {
        galay::utils::CircuitBreakerConfig config;
        config.failureThreshold = 1;
        config.successThreshold = iterations + 1;
        config.halfOpenMaxRequests = 1;
        config.resetTimeout = std::chrono::seconds::zero();
        galay::utils::CircuitBreaker breaker(config);
        breaker.forceOpen();
        (void)breaker.allowRequest();

        printResult(measure("half-open probe success", iterations, [&](std::size_t) {
            breaker.onSuccess();
            const bool allowed = breaker.allowRequest();
            return allowed ? 1u : 0u;
        }));
    }

    std::cout << "\nConcurrent shared CircuitBreaker benchmark\n";
    std::cout << "Threads=" << threadCount
              << ", iterations/thread=" << iterationsPerThread
              << ", total=" << threadCount * iterationsPerThread << '\n';
    std::cout << std::left << std::setw(28) << "Scenario"
              << std::right << std::setw(12) << "ns/op"
              << std::setw(14) << "Mops/s" << '\n';

    {
        galay::utils::CircuitBreakerConfig config;
        config.failureThreshold = threadCount * iterationsPerThread + 1;
        galay::utils::CircuitBreaker breaker(config);
        printResult(measureConcurrent(
            "mt shared closed success",
            threadCount,
            iterationsPerThread,
            [&](std::size_t threadIndex, std::size_t i) {
                auto result = breaker.execute([&]() -> ExpectedInt {
                    return static_cast<int>((threadIndex + i) & 0xffu);
                });
                return static_cast<std::uint64_t>(*result);
            }));
    }

    {
        galay::utils::CircuitBreakerConfig config;
        config.failureThreshold = threadCount * iterationsPerThread + 1;
        galay::utils::CircuitBreaker breaker(config);
        printResult(measureConcurrent(
            "mt shared closed failure",
            threadCount,
            iterationsPerThread,
            [&](std::size_t, std::size_t) {
                auto result = breaker.execute([]() -> ExpectedInt {
                    return std::unexpected(galay::utils::CircuitBreakerError::Open);
                });
                return result.has_value() ? 1u : 0u;
            }));
    }

    {
        galay::utils::CircuitBreakerConfig config;
        config.resetTimeout = std::chrono::hours(1);
        galay::utils::CircuitBreaker breaker(config);
        breaker.forceOpen();
        printResult(measureConcurrent(
            "mt shared open reject",
            threadCount,
            iterationsPerThread,
            [&](std::size_t, std::size_t) {
                auto result = breaker.execute([]() -> ExpectedInt {
                    return 1;
                });
                return result.has_value() ? 1u : 0u;
            }));
    }

    {
        galay::utils::CircuitBreakerConfig config;
        config.failureThreshold = 1;
        config.successThreshold = threadCount * iterationsPerThread + 1;
        config.halfOpenMaxRequests = 1;
        config.resetTimeout = std::chrono::seconds::zero();
        galay::utils::CircuitBreaker breaker(config);
        breaker.forceOpen();
        (void)breaker.allowRequest();
        breaker.onSuccess();

        printResult(measureConcurrent(
            "mt half-open contended",
            threadCount,
            iterationsPerThread,
            [&](std::size_t, std::size_t) {
                const bool allowed = breaker.allowRequest();
                if (allowed) {
                    breaker.onSuccess();
                }
                return allowed ? 1u : 0u;
            }));
    }

    return static_cast<int>(g_sink == static_cast<std::uint64_t>(-1));
}
