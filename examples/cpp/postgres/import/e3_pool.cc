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
    galay::postgres::PostgresConnectionPoolConfig pool_config;
    pool_config.postgres_config = galay::postgres::PostgresConfig::create(
        config.host, config.port, config.user, config.password, config.database);
    pool_config.min_connections = 0;
    pool_config.max_connections = 2;
    galay::postgres::PostgresConnectionPool pool(scheduler, std::move(pool_config));
    auto lease = co_await pool.lease();
    (void)lease;
    done->store(true, std::memory_order_release);
}

} // namespace

int main()
{
    galay::kernel::Runtime runtime;
    const auto started = runtime.start();
    if (!started) return 1;
    auto* scheduler = runtime.getNextIOScheduler();
    std::atomic<bool> done{false};
    if (scheduler == nullptr ||
        !galay::kernel::scheduleTask(
            scheduler,
            run(scheduler, &done, postgres_example::loadConfig()))) {
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
