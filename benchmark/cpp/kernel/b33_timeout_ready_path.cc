/**
 * @file b33_timeout_ready_path.cc
 * @brief 对比 timeout 包装器 ready 快路径与真正挂起路径的固定开销。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kIterations = 1'000'000;

double opsPerSecond(std::chrono::steady_clock::duration elapsed,
                    std::size_t operations) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return ns > 0
        ? static_cast<double>(operations) * 1'000'000'000.0 / static_cast<double>(ns)
        : 0.0;
}

bool benchReadyPath() {
    std::size_t completed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kIterations; ++i) {
        auto wrapped = ReadyAwaitable<int>(static_cast<int>(i)).timeout(5ms);
        if (!wrapped.await_ready() || wrapped.m_timer) {
            std::cerr << "ready path unexpectedly created a timer\n";
            return false;
        }
        auto result = wrapped.await_resume();
        if (result != static_cast<int>(i)) {
            std::cerr << "ready path result mismatch\n";
            return false;
        }
        ++completed;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::cout << "TimeoutReadyPath iterations=" << completed
              << " ops_per_sec=" << std::fixed << std::setprecision(0)
              << opsPerSecond(elapsed, completed) << "\n";
    return true;
}

void benchEagerTimerControl() {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kIterations; ++i) {
        auto timer = std::make_shared<TimeoutTimer>(5ms);
        timer.reset();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::cout << "EagerTimerControl iterations=" << kIterations
              << " ops_per_sec=" << std::fixed << std::setprecision(0)
              << opsPerSecond(elapsed, kIterations) << "\n";
}

}  // namespace

int main() {
    benchEagerTimerControl();
    if (!benchReadyPath()) {
        return 1;
    }
    std::cout << "B33-TimeoutReadyPath PASS\n";
    return 0;
}
