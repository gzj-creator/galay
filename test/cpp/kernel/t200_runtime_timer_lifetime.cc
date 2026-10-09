#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/timer_scheduler.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>

#include <chrono>
#include <iostream>

using namespace galay::kernel;

Task<void> wait_for_timer()
{
    co_await sleep(std::chrono::milliseconds(20));
}

int main()
{
    Runtime server = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    Runtime client = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    const auto server_started = server.start();
    const auto client_started = client.start();
    if (!server_started || !client_started) {
        std::cerr << "runtime startup failed\n";
        return 1;
    }
    client.stop();
    if (!TimerScheduler::get_instance()->is_running()) {
        std::cerr << "stopping one runtime stopped another runtime's timers\n";
        return 1;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto result = server.block_on_io(wait_for_timer());
    if (!result || std::chrono::steady_clock::now() - start < std::chrono::milliseconds(15)) {
        std::cerr << "remaining runtime sleep did not wait\n";
        return 1;
    }
    server.stop();
    if (TimerScheduler::get_instance()->is_running()) {
        std::cerr << "last runtime did not stop timers\n";
        return 1;
    }
    return 0;
}
