/**
 * @file b15_task_timeout_contention.cc
 * @brief 压测 spawnBlocking join/error 交付与 WithTimeout 裁决热路径。
 *
 * 关键覆盖点：
 * - blocking callable 在成功和异常路径下的提交、完成、join 吞吐。
 * - timeout 与 IO completion 竞态裁决在 completion-wins / timeout-wins 两个分支下的开销。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "test/cpp/common/stdout_log.h"

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kBlockingIterations = 20000;
constexpr std::size_t kTimeoutIterations = 200000;

struct Sample {
    double elapsed_ms = 0.0;
    double ops_per_sec = 0.0;
};

Sample makeSample(std::size_t operations,
                  std::chrono::steady_clock::duration elapsed)
{
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return {
        .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
        .ops_per_sec = elapsed_ns > 0
            ? static_cast<double>(operations) * 1'000'000'000.0 / static_cast<double>(elapsed_ns)
            : 0.0,
    };
}

Runtime makeRuntime()
{
    return RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(1)
        .build();
}

void benchSpawnBlockingSuccess()
{
    Runtime runtime = makeRuntime();
    auto started = runtime.start();
    if (!started.has_value()) {
        throw std::runtime_error("runtime failed to start");
    }

    std::vector<JoinHandle<int>> handles;
    handles.reserve(kBlockingIterations);

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kBlockingIterations; ++i) {
        auto handle = runtime.spawnBlocking([i]() {
            return static_cast<int>(i & 0x7f);
        });
        if (!handle.has_value()) {
            throw std::runtime_error("spawnBlocking success submit failed");
        }
        handles.push_back(std::move(*handle));
    }

    std::size_t completed = 0;
    for (auto& handle : handles) {
        auto result = handle.join();
        if (!result.has_value()) {
            throw std::runtime_error("spawnBlocking success join failed");
        }
        ++completed;
    }
    const auto sample = makeSample(completed, std::chrono::steady_clock::now() - start);
    runtime.stop();

    LogInfo("[SpawnBlockingSuccess] tasks={}, time={}ms, throughput={:.0f} joins/s",
            completed,
            sample.elapsed_ms,
            sample.ops_per_sec);
}

void benchSpawnBlockingException()
{
    Runtime runtime = makeRuntime();
    auto started = runtime.start();
    if (!started.has_value()) {
        throw std::runtime_error("runtime failed to start");
    }

    std::vector<JoinHandle<int>> handles;
    handles.reserve(kBlockingIterations);

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kBlockingIterations; ++i) {
        auto handle = runtime.spawnBlocking([]() -> int {
            throw std::runtime_error("expected benchmark exception");
        });
        if (!handle.has_value()) {
            throw std::runtime_error("spawnBlocking exception submit failed");
        }
        handles.push_back(std::move(*handle));
    }

    std::size_t completed = 0;
    for (auto& handle : handles) {
        auto result = handle.join();
        if (result.has_value() ||
            result.error().code() != detail::TaskResultErrorCode::kTaskException) {
            throw std::runtime_error("spawnBlocking exception join mismatch");
        }
        ++completed;
    }
    const auto sample = makeSample(completed, std::chrono::steady_clock::now() - start);
    runtime.stop();

    LogInfo("[SpawnBlockingException] tasks={}, time={}ms, throughput={:.0f} joins/s",
            completed,
            sample.elapsed_ms,
            sample.ops_per_sec);
}

auto makeTimedRecv(IOController& controller, char* buffer)
{
    return RecvAwaitable(&controller, buffer, 1).timeout(1ms);
}

void benchTimeoutCompletionWins()
{
    std::size_t completed = 0;
    const auto start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < kTimeoutIterations; ++i) {
        char buffer = 0;
        IOController controller(GHandle{.fd = -1});
        auto awaitable = makeTimedRecv(controller, &buffer);

        if (!controller.fillAwaitable(RECV, &awaitable.m_inner)) {
            throw std::runtime_error("failed to fill recv awaitable");
        }
        awaitable.m_inner.m_result = size_t{1};
        controller.removeAwaitable(RECV);
        awaitable.m_timer->handleTimeout();

        auto result = awaitable.await_resume();
        if (!result.has_value() || *result != 1) {
            throw std::runtime_error("completion-wins arbitration failed");
        }
        ++completed;
    }

    const auto sample = makeSample(completed, std::chrono::steady_clock::now() - start);
    LogInfo("[TimeoutCompletionWins] iterations={}, time={}ms, throughput={:.0f} ops/s",
            completed,
            sample.elapsed_ms,
            sample.ops_per_sec);
}

void benchTimeoutWins()
{
    std::size_t completed = 0;
    const auto start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < kTimeoutIterations; ++i) {
        char buffer = 0;
        IOController controller(GHandle{.fd = -1});
        auto awaitable = makeTimedRecv(controller, &buffer);

        if (!controller.fillAwaitable(RECV, &awaitable.m_inner)) {
            throw std::runtime_error("failed to fill recv awaitable");
        }
        awaitable.m_timer->handleTimeout();

        auto result = awaitable.await_resume();
        if (result.has_value() || !IOError::contains(result.error().code(), kTimeout)) {
            throw std::runtime_error("timeout-wins arbitration failed");
        }
        ++completed;
    }

    const auto sample = makeSample(completed, std::chrono::steady_clock::now() - start);
    LogInfo("[TimeoutWins] iterations={}, time={}ms, throughput={:.0f} ops/s",
            completed,
            sample.elapsed_ms,
            sample.ops_per_sec);
}

}  // namespace

int main()
{
    benchSpawnBlockingSuccess();
    benchSpawnBlockingException();
    benchTimeoutCompletionWins();
    benchTimeoutWins();

    std::cout << "B15-TaskTimeoutContention PASS\n";
    return 0;
}
