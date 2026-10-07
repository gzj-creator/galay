#include "config.h"

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-postgres/async/client.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <utility>

using namespace std::chrono_literals;

namespace
{

using galay::kernel::IOScheduler;
using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::kernel::Task;
using galay::postgres::AsyncPostgresClient;
using galay::postgres::PostgresConfig;

Task<int> run_client(IOScheduler* scheduler, PostgresConfig config)
{
    AsyncPostgresClient<> client(scheduler);
    auto connected = co_await client.connect(std::move(config)).timeout(5s);
    if (!connected || !connected->has_value() || !connected->value() || client.is_closed()) {
        std::cerr << "PostgreSQL connect failed";
        if (!connected) {
            std::cerr << ": " << connected.error().message();
        }
        std::cerr << '\n';
        co_return 1;
    }

    auto selected = co_await client.query(
        "SELECT id, label, nullable_value "
        "FROM (VALUES (1, 'alpha'::text, NULL::text), "
        "             (2, 'beta'::text, 'present'::text)) "
        "AS rows(id, label, nullable_value) ORDER BY id").timeout(5s);
    if (!selected || !selected->has_value()) {
        std::cerr << "multi-row query failed";
        if (!selected) {
            std::cerr << ": " << selected.error().message();
        }
        std::cerr << '\n';
        co_return 2;
    }

    const auto& rows = selected->value();
    if (rows.field_count() != 3 || rows.row_count() != 2 ||
        rows.row(0).get_int64(0, -1) != 1 || rows.row(0).get_string(1) != "alpha" ||
        !rows.row(0).is_null(2) || rows.row(1).get_int64(0, -1) != 2 ||
        rows.row(1).get_string(1) != "beta" || rows.row(1).is_null(2) ||
        rows.row(1).get_string(2) != "present") {
        std::cerr << "multi-row/NULL result did not match\n";
        co_return 3;
    }

    auto failed = co_await client.query(
        "SELECT * FROM galay_postgres_t5_relation_that_does_not_exist").timeout(5s);
    if (failed || failed.error().sql_state() != "42P01" ||
        client.transaction_status() != 'I') {
        std::cerr << "expected SQLSTATE 42P01 followed by ReadyForQuery(I)\n";
        co_return 4;
    }

    auto recovered = co_await client.query("SELECT 42 AS recovered").timeout(5s);
    if (!recovered || !recovered->has_value() || recovered->value().row_count() != 1 ||
        recovered->value().row(0).get_int64(0, -1) != 42) {
        std::cerr << "connection was not reusable after ErrorResponse\n";
        co_return 5;
    }

    auto closed = co_await client.close();
    if (!closed || !client.is_closed()) {
        std::cerr << "PostgreSQL close failed\n";
        co_return 6;
    }
    co_return 0;
}

} // namespace

int main()
{
    auto config = galay::postgres::test::integration_config();
    if (!config) {
        std::cerr << "t5_client skipped: set GALAY_IT_ENABLE=1 and "
                     "GALAY_POSTGRES_TEST_{HOST,PORT,USER,PASSWORD,DATABASE}.\n";
        return 125;
    }

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    auto started = runtime.start();
    if (!started) {
        std::cerr << "runtime start failed: " << started.error().message() << '\n';
        return EXIT_FAILURE;
    }
    IOScheduler* scheduler = runtime.get_next_io_scheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        std::cerr << "runtime has no IO scheduler\n";
        return EXIT_FAILURE;
    }

    auto result = runtime.block_on_io(run_client(scheduler, std::move(*config)));
    runtime.stop();
    if (!result) {
        std::cerr << "runtime blockOn failed: " << result.error().message() << '\n';
        return EXIT_FAILURE;
    }
    return *result;
}
