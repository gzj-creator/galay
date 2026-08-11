#include "common/config.h"

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-postgres/async/conn_pool.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace
{

struct State
{
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
};

galay::kernel::Task<void> run(galay::kernel::IOScheduler* scheduler,
                              State* state,
                              postgres_example::DbConfig config)
{
    galay::postgres::PostgresConnectionPoolConfig pool_config;
    pool_config.postgres_config = galay::postgres::PostgresConfig::create(config.host,
                                                                          config.port,
                                                                          config.user,
                                                                          config.password,
                                                                          config.database);
    pool_config.min_connections = 0;
    pool_config.max_connections = 4;
    galay::postgres::PostgresConnectionPool pool(scheduler, std::move(pool_config));

    auto acquired = co_await pool.lease();
    if (!acquired || !acquired->has_value()) {
        state->ok = false;
        state->error = !acquired ? acquired.error().message() : "pool acquire produced no lease";
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    galay::postgres::PostgresPoolLease lease = std::move(acquired->value());
    auto result = co_await lease->query("SELECT pg_backend_pid()");
    if (!result || !result->has_value()) {
        state->ok = false;
        state->error = !result ? result.error().message() : "pool query produced no result";
    } else if (result->value().rowCount() != 0) {
        std::cout << "backend pid: " << result->value().row(0).getString(0) << '\n';
    }
    state->done.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    galay::kernel::Runtime runtime;
    const auto started = runtime.start();
    if (!started) {
        std::cerr << started.error().message() << '\n';
        return 1;
    }
    auto* scheduler = runtime.getNextIOScheduler();
    State state;
    if (scheduler == nullptr ||
        !galay::kernel::scheduleTask(
            scheduler,
            run(scheduler, &state, postgres_example::loadConfig()))) {
        runtime.stop();
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!state.done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    runtime.stop();
    if (!state.done.load(std::memory_order_acquire) || !state.ok) {
        std::cerr << (state.error.empty() ? "pool example timed out" : state.error) << '\n';
        return 1;
    }
    return 0;
}
