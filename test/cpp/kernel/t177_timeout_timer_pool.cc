/**
 * @file t177_timeout_timer_pool.cc
 * @brief 验证 TimeoutTimer 池的复用和不抛异常释放契约。
 */

#include <galay/cpp/galay-kernel/core/timeout.hpp>
#include <cassert>
#include <chrono>
#include <iostream>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

using namespace galay::kernel;
using namespace std::chrono_literals;

int main()
{
    static_assert(noexcept(std::declval<detail::TimeoutTimerPool&>().release(nullptr)));

    auto first = TimeoutTimer::create(1ms);
    auto* first_address = first.get();
    first.reset();

    auto reused = TimeoutTimer::create(2ms);
    assert(reused.get() == first_address);

    std::vector<TimeoutTimer::ptr> timers;
    timers.reserve(1024);
    for (size_t i = 0; i < 1024; ++i) {
        timers.push_back(TimeoutTimer::create(1ms));
    }
    timers.clear();

    auto cross_thread = TimeoutTimer::create(1ms);
    std::thread releaser([timer = std::move(cross_thread)]() mutable {
        timer.reset();
    });
    releaser.join();

    std::cout << "T177-TimeoutTimerPool PASS\n";
    return 0;
}
