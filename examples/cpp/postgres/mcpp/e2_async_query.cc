#include "common/config.h"

#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <thread>

import galay.postgres;

namespace
{

galay::kernel::Task<void> run(galay::kernel::IOScheduler* scheduler,
                              std::atomic<bool>* done,
                              postgres_example::DbConfig config)
{
    galay::postgres::AsyncPostgresClient<> client(scheduler);
    auto connected = co_await client.connect(config.host, config.port, config.user,
                                             config.password, config.database);
    if (connected && connected->has_value()) {
        auto result = co_await client.query("SELECT 1");
        (void)result;
    }
    done->store(true, std::memory_order_release);
}

} // namespace

int main()
{
    galay::kernel::Runtime runtime;
    const auto started = runtime.start();
    if (!started) return 1;
    auto* scheduler = runtime.get_next_io_scheduler();
    std::atomic<bool> done{false};
    if (scheduler == nullptr ||
        !galay::kernel::schedule_task(
            scheduler,
            run(scheduler, &done, postgres_example::load_config()))) {
        runtime.stop();
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    runtime.stop();
    return done.load(std::memory_order_acquire) ? 0 : 1;
}
