#include <galay/cpp/galay-redis/async/conn_pool_waiter_state.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>

using galay::redis::detail::PoolWaiterState;
using galay::redis::detail::try_complete_waiter;

namespace
{

constexpr std::uint64_t kIterations = 2'000'000;

struct Counts
{
    std::uint64_t completed = 0;
    std::uint64_t timed_out = 0;
    std::uint64_t duplicate_ignored = 0;
};

Counts runPressure()
{
    Counts counts;
    for (std::uint64_t i = 0; i < kIterations; ++i) {
        std::atomic<PoolWaiterState> state{PoolWaiterState::Waiting};
        const bool release_first = (i & 1U) == 0;

        if (release_first) {
            if (try_complete_waiter(state, PoolWaiterState::Completed)) {
                ++counts.completed;
            }
            if (!try_complete_waiter(state, PoolWaiterState::TimedOut)) {
                ++counts.duplicate_ignored;
            }
        } else {
            if (try_complete_waiter(state, PoolWaiterState::TimedOut)) {
                ++counts.timed_out;
            }
            if (!try_complete_waiter(state, PoolWaiterState::Completed)) {
                ++counts.duplicate_ignored;
            }
        }
    }
    return counts;
}

} // namespace

int main()
{
    const auto start = std::chrono::steady_clock::now();
    const Counts counts = runPressure();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    std::cout << "iterations=" << kIterations << '\n';
    std::cout << "completed=" << counts.completed << '\n';
    std::cout << "timed_out=" << counts.timed_out << '\n';
    std::cout << "duplicate_ignored=" << counts.duplicate_ignored << '\n';
    std::cout << "elapsed_us=" << elapsed_us << '\n';

    if (counts.completed != kIterations / 2 ||
        counts.timed_out != kIterations / 2 ||
        counts.duplicate_ignored != kIterations) {
        return 1;
    }

    return 0;
}
