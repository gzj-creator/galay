#include "config.h"

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-postgres/async/conn_pool.h>

#include <chrono>
#include <concepts>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <type_traits>

using namespace std::chrono_literals;
using namespace galay::postgres;

static_assert(!std::is_copy_constructible_v<PostgresConnectionPool>);
static_assert(!std::is_copy_assignable_v<PostgresConnectionPool>);

static_assert(requires(PostgresConnectionPool& pool, AsyncPostgresClient<>* client) {
    { pool.acquire() } -> std::same_as<PostgresConnectionPool::AcquireAwaitable>;
    { pool.lease() } -> std::same_as<PostgresConnectionPool::LeaseAwaitable>;
    { pool.release(client) } -> std::same_as<void>;
    { pool.size() } -> std::same_as<size_t>;
    { pool.idleCount() } -> std::same_as<size_t>;
});

namespace
{

using galay::kernel::IOScheduler;
using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::kernel::Task;

Task<int> runPool(IOScheduler* scheduler, PostgresConfig config)
{
    PostgresConnectionPoolConfig pool_config;
    pool_config.postgres_config = std::move(config);
    pool_config.min_connections = 1;
    pool_config.max_connections = 2;
    PostgresConnectionPool pool(scheduler, std::move(pool_config));

    AsyncPostgresClient<>* first_client = nullptr;
    {
        auto leased = co_await pool.lease();
        if (!leased || !leased->has_value()) {
            co_return 1;
        }
        PostgresPoolLease lease = std::move(leased->value());
        first_client = lease.get();
        auto selected = co_await lease->query("SELECT 101").timeout(5s);
        if (!selected || !selected->has_value() ||
            selected->value().row(0).getInt64(0, -1) != 101) {
            co_return 2;
        }
    }
    if (pool.size() != 1 || pool.idleCount() != 1) {
        co_return 3;
    }

    {
        auto leased = co_await pool.lease();
        if (!leased || !leased->has_value()) {
            co_return 4;
        }
        PostgresPoolLease lease = std::move(leased->value());
        if (lease.get() != first_client) {
            co_return 5;
        }
        auto begun = co_await lease->beginTransaction().timeout(5s);
        if (!begun || !begun->has_value() || lease->transactionStatus() != 'T') {
            co_return 6;
        }
    }
    if (pool.idleCount() != 0) {
        co_return 7;
    }

    {
        auto leased = co_await pool.lease();
        if (!leased || !leased->has_value()) {
            co_return 8;
        }
        PostgresPoolLease lease = std::move(leased->value());
        if (lease.get() != first_client || lease->transactionStatus() != 'I') {
            co_return 9;
        }
        auto selected = co_await lease->query("SELECT 202").timeout(5s);
        if (!selected || !selected->has_value() ||
            selected->value().row(0).getInt64(0, -1) != 202) {
            co_return 10;
        }
    }
    co_return pool.idleCount() == 1 ? 0 : 11;
}

} // namespace

int main()
{
    auto config = galay::postgres::test::integrationConfig();
    if (!config) {
        std::cerr << "t10_pool skipped: set GALAY_IT_ENABLE=1 and "
                     "GALAY_POSTGRES_TEST_{HOST,PORT,USER,PASSWORD,DATABASE}.\n";
        return 125;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).parallelSchedulerCount(0).build();
    auto started = runtime.start();
    if (!started) {
        std::cerr << "runtime start failed: " << started.error().message() << '\n';
        return EXIT_FAILURE;
    }
    IOScheduler* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        return EXIT_FAILURE;
    }

    auto result = runtime.blockOnIO(runPool(scheduler, std::move(*config)));
    runtime.stop();
    if (!result || *result != 0) {
        std::cerr << "pool integration failed at step " << (result ? *result : -1) << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
