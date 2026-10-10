#include <galay/cpp/galay-kernel/common/timer_manager_mt.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

bool check_deadline(std::chrono::milliseconds idle, uint64_t tick_ns,
                    std::chrono::milliseconds delay)
{
    galay::kernel::ThreadSafeTimerManager manager(tick_ns);
    std::this_thread::sleep_for(idle);
    bool fired = false;
    bool early = false;
    const auto submitted = Clock::now();
    auto timer = std::make_shared<galay::kernel::CBTimer>(delay, [&]() {
        early = Clock::now() - submitted < delay;
        fired = true;
    });
    if (!manager.push(std::move(timer))) {
        std::cerr << "timer submission failed\n";
        return false;
    }
    const auto deadline = Clock::now() + 1s;
    do {
        manager.tick();
        if (!fired) {
            std::this_thread::sleep_for(1ms);
        }
    } while (!fired && Clock::now() < deadline);
    if (early || !fired) {
        std::cerr << "timer deadline violated: idle_ms=" << idle.count()
                  << " delay_ms=" << delay.count() << " early=" << early
                  << " fired=" << fired << '\n';
        return false;
    }
    return true;
}

} // namespace

int main()
{
    const bool idle_ok = check_deadline(300ms, 1'000'000ULL, 10ms);
    const bool short_ok = check_deadline(0ms, 10'000'000ULL, 2ms);
    const bool cascade_ok = check_deadline(0ms, 1'000'000ULL, 270ms);
    if (!idle_ok || !short_ok || !cascade_ok) {
        return 1;
    }
    std::cout << "T202-TimerDeadlineAfterIdle PASS\n";
    return 0;
}
