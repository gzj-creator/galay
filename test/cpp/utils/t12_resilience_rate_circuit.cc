#include "test_common.hpp"

#include <barrier>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <variant>

enum class CircuitBreakerTestError {
    OperationFailed
};

struct ConcurrentAcquireResult {
    size_t attempts;
    size_t acquired;
    std::chrono::milliseconds duration;
};

template<typename Acquire>
ConcurrentAcquireResult run_concurrent_acquire(size_t threadCount, size_t attemptsPerThread, Acquire& acquire) {
    std::barrier startLine(static_cast<std::ptrdiff_t>(threadCount + 1));
    std::atomic<size_t> acquired{0};
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        threads.emplace_back([&]() {
            startLine.arrive_and_wait();
            for (size_t i = 0; i < attemptsPerThread; ++i) {
                if (acquire()) {
                    acquired.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    startLine.arrive_and_wait();
    const auto start = std::chrono::high_resolution_clock::now();
    for (auto& thread : threads) {
        thread.join();
    }
    const auto end = std::chrono::high_resolution_clock::now();

    return {
        threadCount * attemptsPerThread,
        acquired.load(std::memory_order_relaxed),
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
    };
}

void test_rate_limiter_uses_lock_free_non_blocking_state() {
    const auto sourceRoot = std::filesystem::path(GALAY_UTILS_SOURCE_DIR);
    std::ifstream input(sourceRoot / "galay-utils/resilience/rate_limiter.hpp");
    assert(input.good());

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const auto rateLimiterStart = source.find("class CountingSemaphore");
    assert(rateLimiterStart != std::string::npos);

    const std::string limiterSource = source.substr(rateLimiterStart);
    assert(limiterSource.find("std::mutex") == std::string::npos);
    assert(limiterSource.find("std::lock_guard") == std::string::npos);
    assert(limiterSource.find("std::unique_lock") == std::string::npos);
    assert(limiterSource.find("std::condition_variable") == std::string::npos);
    assert(limiterSource.find("std::deque") == std::string::npos);
}

void test_rate_limiter() {
    std::cout << "=== Testing RateLimiter ===" << std::endl;

    // Counting semaphore - 使用 try_acquire 测试基本功能
    CountingSemaphore sem(3);
    assert(sem.available() == 3);

    assert(sem.try_acquire(2));
    assert(sem.available() == 1);
    assert(!sem.try_acquire(2));
    assert(sem.available() == 1);

    sem.release(2);
    assert(sem.available() == 3);
    assert(sem.try_acquire(3));
    assert(sem.available() == 0);
    sem.release(3);

    // Token bucket
    TokenBucketLimiter tokenBucket(100, 10); // 100 tokens/sec, capacity 10

    // Should be able to acquire immediately (bucket starts full)
    assert(tokenBucket.try_acquire(5));
    assert(tokenBucket.available_tokens() >= 4); // At least 5 consumed
    assert(tokenBucket.try_acquire(5));
    assert(!tokenBucket.try_acquire(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(tokenBucket.try_acquire(1));
    tokenBucket.set_capacity(3);
    assert(tokenBucket.capacity() == 3);
    assert(tokenBucket.available_tokens() <= 3.0);
    tokenBucket.set_rate(200);
    assert(tokenBucket.rate() == 200);

    // Sliding window
    SlidingWindowLimiter slidingWindow(5, std::chrono::milliseconds(100));
    assert(slidingWindow.max_requests() == 5);
    assert(slidingWindow.window_size() == std::chrono::milliseconds(100));

    for (int i = 0; i < 5; ++i) {
        assert(slidingWindow.try_acquire());
    }
    assert(!slidingWindow.try_acquire()); // Should be rate limited

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(slidingWindow.try_acquire()); // Should work after window expires

    // Leaky bucket
    LeakyBucketLimiter leakyBucket(100, 3);
    assert(leakyBucket.rate() == 100);
    assert(leakyBucket.capacity() == 3);
    assert(leakyBucket.try_acquire(2));
    assert(!leakyBucket.try_acquire(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(leakyBucket.try_acquire(1));
    assert(leakyBucket.current_water() <= 3.0);

    LeakyBucketLimiter slowLeakyBucket(10, 2);
    assert(slowLeakyBucket.try_acquire(2));
    assert(!slowLeakyBucket.try_acquire(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    assert(slowLeakyBucket.try_acquire(1));

    std::cout << "RateLimiter tests passed!" << std::endl;
}

// ==================== CircuitBreaker Tests ====================

void test_circuit_breaker() {
    std::cout << "=== Testing CircuitBreaker ===" << std::endl;

    CircuitBreakerConfig config;
    config.failureThreshold = 3;
    config.successThreshold = 2;
    config.resetTimeout = std::chrono::seconds(1);

    CircuitBreaker cb(config);

    assert(cb.state() == CircuitState::Closed);
    assert(cb.allow_request());

    // Simulate failures
    cb.on_failure();
    cb.on_failure();
    assert(cb.state() == CircuitState::Closed);

    cb.on_failure(); // Third failure
    assert(cb.state() == CircuitState::Open);
    assert(!cb.allow_request());

    // Wait for reset timeout
    std::this_thread::sleep_for(std::chrono::seconds(1));
    assert(cb.allow_request()); // Should transition to half-open
    assert(cb.state() == CircuitState::HalfOpen);

    // Success in half-open
    cb.on_success();
    cb.on_success();
    assert(cb.state() == CircuitState::Closed);

    std::cout << "CircuitBreaker tests passed!" << std::endl;
}

void test_circuit_breaker_expected_execution() {
    std::cout << "=== Testing CircuitBreaker expected execution ===" << std::endl;

    using Error = std::variant<CircuitBreakerTestError, CircuitBreakerError>;
    using Result = std::expected<int, Error>;

    static_assert(CircuitBreakerExpected<Result>);
    static_assert(!CircuitBreakerExpected<int>);

    CircuitBreakerConfig config;
    config.failureThreshold = 2;
    config.successThreshold = 1;
    config.resetTimeout = std::chrono::seconds(1);

    CircuitBreaker cb(config);
    int calls = 0;

    auto success = cb.execute([&]() -> Result {
        ++calls;
        return 7;
    });
    assert(success.has_value());
    assert(*success == 7);
    assert(calls == 1);
    assert(cb.failure_count() == 0);
    assert(cb.state() == CircuitState::Closed);

    auto firstFailure = cb.execute([&]() -> Result {
        ++calls;
        return std::unexpected(Error{CircuitBreakerTestError::OperationFailed});
    });
    assert(!firstFailure.has_value());
    assert(std::holds_alternative<CircuitBreakerTestError>(firstFailure.error()));
    assert(calls == 2);
    assert(cb.failure_count() == 1);
    assert(cb.state() == CircuitState::Closed);

    auto secondFailure = cb.execute([&]() -> Result {
        ++calls;
        return std::unexpected(Error{CircuitBreakerTestError::OperationFailed});
    });
    assert(!secondFailure.has_value());
    assert(calls == 3);
    assert(cb.state() == CircuitState::Open);

    auto blocked = cb.execute([&]() -> Result {
        ++calls;
        return 99;
    });
    assert(!blocked.has_value());
    assert(std::get<CircuitBreakerError>(blocked.error()) == CircuitBreakerError::Open);
    assert(calls == 3);

    std::cout << "CircuitBreaker expected execution tests passed!" << std::endl;
}

void test_circuit_breaker_expected_fallback() {
    std::cout << "=== Testing CircuitBreaker expected fallback ===" << std::endl;

    using Result = std::expected<int, CircuitBreakerTestError>;

    static_assert(CircuitBreakerExpected<Result>);

    CircuitBreakerConfig config;
    config.failureThreshold = 1;
    config.successThreshold = 1;
    config.resetTimeout = std::chrono::seconds(1);

    CircuitBreaker cb(config);
    int primaryCalls = 0;
    int fallbackCalls = 0;

    auto fallbackAfterFailure = cb.execute_with_fallback(
        [&]() -> Result {
            ++primaryCalls;
            return std::unexpected(CircuitBreakerTestError::OperationFailed);
        },
        [&]() -> Result {
            ++fallbackCalls;
            return 42;
        });
    assert(fallbackAfterFailure.has_value());
    assert(*fallbackAfterFailure == 42);
    assert(primaryCalls == 1);
    assert(fallbackCalls == 1);
    assert(cb.state() == CircuitState::Open);

    auto fallbackWhenOpen = cb.execute_with_fallback(
        [&]() -> Result {
            ++primaryCalls;
            return 7;
        },
        [&]() -> Result {
            ++fallbackCalls;
            return 43;
        });
    assert(fallbackWhenOpen.has_value());
    assert(*fallbackWhenOpen == 43);
    assert(primaryCalls == 1);
    assert(fallbackCalls == 2);

    std::cout << "CircuitBreaker expected fallback tests passed!" << std::endl;
}

void test_circuit_breaker_manual_clock_timeout() {
    std::cout << "=== Testing CircuitBreaker manual clock timeout ===" << std::endl;

    ManualClock::reset();

    CircuitBreakerConfig config;
    config.failureThreshold = 1;
    config.successThreshold = 1;
    config.resetTimeout = std::chrono::seconds(1);

    BasicCircuitBreaker<ManualClock> cb(config);

    cb.on_failure();
    assert(cb.state() == CircuitState::Open);

    ManualClock::advance(std::chrono::milliseconds(999));
    assert(!cb.allow_request());
    assert(cb.state() == CircuitState::Open);

    ManualClock::advance(std::chrono::milliseconds(1));
    assert(cb.allow_request());
    assert(cb.state() == CircuitState::HalfOpen);

    std::cout << "CircuitBreaker manual clock timeout tests passed!" << std::endl;
}

void test_circuit_breaker_half_open_probe_limit() {
    std::cout << "=== Testing CircuitBreaker half-open probe limit ===" << std::endl;

    ManualClock::reset();

    CircuitBreakerConfig config;
    config.failureThreshold = 1;
    config.successThreshold = 2;
    config.resetTimeout = std::chrono::seconds(1);
    config.halfOpenMaxRequests = 1;

    BasicCircuitBreaker<ManualClock> cb(config);

    cb.on_failure();
    assert(cb.state() == CircuitState::Open);

    ManualClock::advance(std::chrono::seconds(1));
    assert(cb.allow_request());
    assert(cb.state() == CircuitState::HalfOpen);
    assert(!cb.allow_request());

    cb.on_success();
    assert(cb.state() == CircuitState::HalfOpen);
    assert(cb.allow_request());

    cb.on_success();
    assert(cb.state() == CircuitState::Closed);

    std::cout << "CircuitBreaker half-open probe limit tests passed!" << std::endl;
}

void test_circuit_breaker_force_open_uses_current_time() {
    std::cout << "=== Testing CircuitBreaker force open timestamp ===" << std::endl;

    ManualClock::reset();

    CircuitBreakerConfig config;
    config.failureThreshold = 1;
    config.successThreshold = 1;
    config.resetTimeout = std::chrono::seconds(1);

    BasicCircuitBreaker<ManualClock> cb(config);

    ManualClock::advance(std::chrono::seconds(10));
    cb.force_open();
    assert(cb.state() == CircuitState::Open);

    ManualClock::advance(std::chrono::milliseconds(999));
    assert(!cb.allow_request());
    assert(cb.state() == CircuitState::Open);

    ManualClock::advance(std::chrono::milliseconds(1));
    assert(cb.allow_request());
    assert(cb.state() == CircuitState::HalfOpen);

    std::cout << "CircuitBreaker force open timestamp tests passed!" << std::endl;
}

// ==================== ConsistentHash Tests ====================

void test_stress_circuit_breaker() {
    std::cout << "=== Stress Testing CircuitBreaker ===" << std::endl;

    CircuitBreakerConfig config;
    config.failureThreshold = 100;
    config.successThreshold = 50;
    config.resetTimeout = std::chrono::seconds(1);

    CircuitBreaker cb(config);

    const int numThreads = 4;
    const int opsPerThread = 5000;
    std::atomic<int> successOps{0};
    std::atomic<int> failureOps{0};
    std::atomic<int> allowedRequests{0};

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < opsPerThread; ++i) {
                if (cb.allow_request()) {
                    ++allowedRequests;
                    if (i % 10 == 0) {
                        cb.on_failure();
                        ++failureOps;
                    } else {
                        cb.on_success();
                        ++successOps;
                    }
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    int totalOps = numThreads * opsPerThread;
    double opsPerSec = (totalOps * 1000.0) / duration;

    std::cout << "  Threads: " << numThreads << std::endl;
    std::cout << "  Total ops: " << totalOps << std::endl;
    std::cout << "  Duration: " << duration << "ms" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(0) << opsPerSec << " ops/sec" << std::endl;
    std::cout << "  Allowed requests: " << allowedRequests << std::endl;
    std::cout << "  Success ops: " << successOps << std::endl;
    std::cout << "  Failure ops: " << failureOps << std::endl;
    std::cout << "  Final state: " << cb.state_string() << std::endl;

    std::cout << "CircuitBreaker stress test passed!" << std::endl;
}

void test_stress_rate_limiter_correctness() {
    std::cout << "=== Stress Testing RateLimiter correctness ===" << std::endl;

    constexpr size_t threadCount = 16;
    constexpr size_t attemptsPerThread = 2048;
    constexpr size_t limit = 2048;

    {
        CountingSemaphore limiter(limit);
        auto acquire = [&]() {
            return limiter.try_acquire();
        };

        const auto result = run_concurrent_acquire(threadCount, attemptsPerThread, acquire);

        std::cout << "  [CountingSemaphore - Exact Limit]" << std::endl;
        std::cout << "    Attempts: " << result.attempts << ", Acquired: " << result.acquired << std::endl;
        std::cout << "    Duration: " << result.duration.count() << "ms" << std::endl;

        assert(result.acquired == limit);
        assert(limiter.available() == 0);
        assert(!limiter.try_acquire());
    }

    {
        TokenBucketLimiter limiter(0, limit);
        auto acquire = [&]() {
            return limiter.try_acquire();
        };

        const auto result = run_concurrent_acquire(threadCount, attemptsPerThread, acquire);

        std::cout << "  [TokenBucketLimiter - Exact Capacity]" << std::endl;
        std::cout << "    Attempts: " << result.attempts << ", Acquired: " << result.acquired << std::endl;
        std::cout << "    Duration: " << result.duration.count() << "ms" << std::endl;

        assert(result.acquired == limit);
        assert(limiter.available_tokens() == 0.0);
        assert(!limiter.try_acquire());
    }

    {
        SlidingWindowLimiter limiter(limit, std::chrono::hours(1));
        auto acquire = [&]() {
            return limiter.try_acquire();
        };

        const auto result = run_concurrent_acquire(threadCount, attemptsPerThread, acquire);

        std::cout << "  [SlidingWindowLimiter - Exact Window]" << std::endl;
        std::cout << "    Attempts: " << result.attempts << ", Acquired: " << result.acquired << std::endl;
        std::cout << "    Duration: " << result.duration.count() << "ms" << std::endl;

        assert(result.acquired == limit);
        assert(!limiter.try_acquire());
    }

    {
        LeakyBucketLimiter limiter(0, limit);
        auto acquire = [&]() {
            return limiter.try_acquire();
        };

        const auto result = run_concurrent_acquire(threadCount, attemptsPerThread, acquire);

        std::cout << "  [LeakyBucketLimiter - Exact Capacity]" << std::endl;
        std::cout << "    Attempts: " << result.attempts << ", Acquired: " << result.acquired << std::endl;
        std::cout << "    Duration: " << result.duration.count() << "ms" << std::endl;

        assert(result.acquired == limit);
        assert(limiter.current_water() == static_cast<double>(limit));
        assert(!limiter.try_acquire());
    }

    std::cout << "RateLimiter correctness stress test passed!" << std::endl;
}

void test_stress_rate_limiter() {
    std::cout << "=== Stress Testing RateLimiter ===" << std::endl;

    const int threadCount = 4;
    const int iterations = 5000;

    // Test CountingSemaphore
    {
        CountingSemaphore sem(100);
        std::atomic<int> acquired{0};

        auto start = std::chrono::high_resolution_clock::now();

        std::vector<std::thread> threads;
        for (int thread_index = 0; thread_index < threadCount; ++thread_index) {
            threads.emplace_back([&]() {
                for (int i = 0; i < iterations; ++i) {
                    if (sem.try_acquire(1)) {
                        ++acquired;
                        sem.release(1);
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        int totalOps = acquired.load();
        double opsPerSec = duration > 0 ? (totalOps * 1000.0) / duration : 0;

        std::cout << "  [CountingSemaphore - Thread]" << std::endl;
        std::cout << "    Threads: " << threadCount << ", Iterations: " << threadCount * iterations << std::endl;
        std::cout << "    Duration: " << duration << "ms" << std::endl;
        std::cout << "    Throughput: " << std::fixed << std::setprecision(0) << opsPerSec << " ops/sec" << std::endl;
        std::cout << "    Acquired: " << acquired << std::endl;
        assert(sem.available() == 100);
    }

    // Test TokenBucketLimiter
    {
        TokenBucketLimiter limiter(10000000, 100000); // 10M tokens/sec, capacity 100000
        std::atomic<int> acquired{0};

        auto start = std::chrono::high_resolution_clock::now();

        std::vector<std::thread> threads;
        for (int thread_index = 0; thread_index < threadCount; ++thread_index) {
            threads.emplace_back([&]() {
                for (int i = 0; i < iterations; ++i) {
                    if (limiter.try_acquire(1)) {
                        ++acquired;
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        int totalOps = threadCount * iterations;
        double opsPerSec = duration > 0 ? (totalOps * 1000.0) / duration : 0;

        std::cout << "  [TokenBucketLimiter - Thread]" << std::endl;
        std::cout << "    Threads: " << threadCount << ", Iterations: " << totalOps << std::endl;
        std::cout << "    Duration: " << duration << "ms" << std::endl;
        std::cout << "    Throughput: " << std::fixed << std::setprecision(0) << opsPerSec << " ops/sec" << std::endl;
        std::cout << "    Acquired: " << acquired << std::endl;
    }

    // Test SlidingWindowLimiter
    {
        SlidingWindowLimiter limiter(1000000, std::chrono::milliseconds(1000));
        std::atomic<int> acquired{0};

        auto start = std::chrono::high_resolution_clock::now();

        std::vector<std::thread> threads;
        for (int thread_index = 0; thread_index < threadCount; ++thread_index) {
            threads.emplace_back([&]() {
                for (int i = 0; i < iterations / 2; ++i) {
                    if (limiter.try_acquire()) {
                        ++acquired;
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        int totalOps = threadCount * (iterations / 2);
        double opsPerSec = duration > 0 ? (totalOps * 1000.0) / duration : 0;

        std::cout << "  [SlidingWindowLimiter - Thread]" << std::endl;
        std::cout << "    Threads: " << threadCount << ", Iterations: " << totalOps << std::endl;
        std::cout << "    Duration: " << duration << "ms" << std::endl;
        std::cout << "    Throughput: " << std::fixed << std::setprecision(0) << opsPerSec << " ops/sec" << std::endl;
        std::cout << "    Acquired: " << acquired << std::endl;
    }

    std::cout << "RateLimiter stress test passed!" << std::endl;
}

int main() {
    std::cout << "\n=== resilience_test ===" << std::endl;
    try {
        test_rate_limiter_uses_lock_free_non_blocking_state();
        test_rate_limiter();
        test_circuit_breaker();
        test_circuit_breaker_expected_execution();
        test_circuit_breaker_expected_fallback();
        test_circuit_breaker_manual_clock_timeout();
        test_circuit_breaker_half_open_probe_limit();
        test_circuit_breaker_force_open_uses_current_time();
        test_stress_circuit_breaker();
        test_stress_rate_limiter_correctness();
        test_stress_rate_limiter();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
