#include "config.h"

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-postgres/async/client.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace
{

using galay::kernel::IOScheduler;
using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::kernel::Task;
using galay::postgres::AsyncPostgresClient;
using galay::postgres::PostgresConfig;
using galay::postgres::PostgresOid;

Task<int> runPrepared(IOScheduler* scheduler, PostgresConfig config)
{
    AsyncPostgresClient<> client(scheduler);
    auto connected = co_await client.connect(std::move(config)).timeout(5s);
    if (!connected || !connected->has_value() || !connected->value()) {
        std::cerr << "PostgreSQL connect failed\n";
        co_return 1;
    }

    constexpr std::string_view kStatement = "galay_t7_prepared_values";
    auto prepared = co_await client.prepare(
        kStatement,
        "SELECT $1::text AS text_value, $2::int4 AS int_value, "
        "       $3::text IS NULL AS was_null, $3::text AS nullable_value").timeout(5s);
    if (!prepared || !prepared->has_value()) {
        std::cerr << "prepare failed";
        if (!prepared) {
            std::cerr << ": " << prepared.error().message();
        }
        std::cerr << '\n';
        co_return 2;
    }

    const auto& metadata = prepared->value();
    if (metadata.statement_name != kStatement || metadata.parameter_types.size() != 3 ||
        metadata.parameter_types[0] != static_cast<uint32_t>(PostgresOid::TEXT) ||
        metadata.parameter_types[1] != static_cast<uint32_t>(PostgresOid::INT4) ||
        metadata.parameter_types[2] != static_cast<uint32_t>(PostgresOid::TEXT) ||
        metadata.fields.size() != 4) {
        std::cerr << "prepared statement metadata did not match\n";
        co_return 3;
    }

    std::vector<std::optional<std::string>> null_params{
        std::string("hello"), std::string("42"), std::nullopt};
    auto null_result = co_await client.execute(
        kStatement,
        std::span<const std::optional<std::string>>(null_params)).timeout(5s);
    if (!null_result || !null_result->has_value() || null_result->value().rowCount() != 1) {
        std::cerr << "prepared execution with NULL failed\n";
        co_return 4;
    }
    const auto& null_row = null_result->value().row(0);
    if (null_row.getString(0) != "hello" || null_row.getInt64(1, -1) != 42 ||
        null_row.getString(2) != "t" || !null_row.isNull(3)) {
        std::cerr << "prepared NULL result did not match\n";
        co_return 5;
    }

    std::vector<std::optional<std::string>> empty_params{
        std::string("world"), std::string("7"), std::string()};
    auto empty_result = co_await client.execute(
        kStatement,
        std::span<const std::optional<std::string>>(empty_params)).timeout(5s);
    if (!empty_result || !empty_result->has_value() || empty_result->value().rowCount() != 1) {
        std::cerr << "prepared execution with empty text failed\n";
        co_return 6;
    }
    const auto& empty_row = empty_result->value().row(0);
    if (empty_row.getString(0) != "world" || empty_row.getInt64(1, -1) != 7 ||
        empty_row.getString(2) != "f" || empty_row.isNull(3) ||
        empty_row.getString(3) != "") {
        std::cerr << "empty text was not kept distinct from SQL NULL\n";
        co_return 7;
    }

    auto closed = co_await client.close();
    if (!closed) {
        std::cerr << "PostgreSQL close failed\n";
        co_return 8;
    }
    co_return 0;
}

} // namespace

int main()
{
    auto config = galay::postgres::test::integrationConfig();
    if (!config) {
        std::cerr << "t7_prepared skipped: set GALAY_IT_ENABLE=1 and "
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
        std::cerr << "runtime has no IO scheduler\n";
        return EXIT_FAILURE;
    }

    auto result = runtime.blockOnIO(runPrepared(scheduler, std::move(*config)));
    runtime.stop();
    if (!result) {
        std::cerr << "runtime blockOn failed: " << result.error().message() << '\n';
        return EXIT_FAILURE;
    }
    return *result;
}
