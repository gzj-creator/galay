#include "config.h"

#include <galay/cpp/galay-postgres/sync/postgres_client.h>

#include <concepts>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace galay::postgres;

static_assert(!std::is_copy_constructible_v<PostgresClient>);
static_assert(!std::is_copy_assignable_v<PostgresClient>);
static_assert(std::is_nothrow_move_constructible_v<PostgresClient>);
static_assert(std::is_nothrow_move_assignable_v<PostgresClient>);

static_assert(requires(PostgresClient& client,
                       PostgresConfig config,
                       std::vector<std::optional<std::string>> params,
                       std::span<const std::string_view> sqls,
                       std::span<const protocol::PostgresCommandView> commands) {
    { client.connect(config) } -> std::same_as<PostgresVoidResult>;
    { client.query("SELECT 1") } -> std::same_as<PostgresResult>;
    { client.prepare("galay_stmt", "SELECT $1") };
    { client.execute("galay_stmt", params) } -> std::same_as<PostgresResult>;
    { client.pipeline(sqls) } -> std::same_as<PostgresBatchResult>;
    { client.batch(commands) } -> std::same_as<PostgresBatchResult>;
    { client.beginTransaction() } -> std::same_as<PostgresVoidResult>;
    { client.commit() } -> std::same_as<PostgresVoidResult>;
    { client.rollback() } -> std::same_as<PostgresVoidResult>;
    { client.transactionStatus() } -> std::same_as<char>;
});

int main()
{
    PostgresClient client;
    auto query_result = client.query("SELECT 1");
    if (query_result || query_result.error().type() != POSTGRES_ERROR_CONNECTION_CLOSED) {
        return 1;
    }

    const std::string invalid_name("bad\0name", 8);
    auto prepare_result = client.prepare(invalid_name, "SELECT 1");
    if (prepare_result || prepare_result.error().type() != POSTGRES_ERROR_INVALID_PARAM) {
        return 2;
    }

    std::vector<std::optional<std::string>> too_many_parameters(32768);
    auto execute_result = client.execute("stmt", too_many_parameters);
    if (execute_result || execute_result.error().type() != POSTGRES_ERROR_INVALID_PARAM) {
        return 3;
    }

    auto close_result = client.closePrepared(invalid_name);
    if (close_result || close_result.error().type() != POSTGRES_ERROR_INVALID_PARAM) {
        return 4;
    }

    auto config = galay::postgres::test::integrationConfig();
    if (!config) {
        std::cerr << "t13_sync_client skipped: set GALAY_IT_ENABLE=1 and "
                     "GALAY_POSTGRES_TEST_{HOST,PORT,USER,PASSWORD,DATABASE}.\n";
        return 125;
    }
    auto connected = client.connect(*config);
    if (!connected) {
        return 5;
    }

    constexpr std::string_view kStatement = "galay_t13_sync_statement";
    auto prepared = client.prepare(
        kStatement,
        "SELECT $1::int4 AS value, $2::text AS nullable_value");
    if (!prepared || prepared->parameter_types.size() != 2 || prepared->fields.size() != 2) {
        return 6;
    }
    std::vector<std::optional<std::string>> parameters{std::string("303"), std::nullopt};
    auto executed = client.execute(kStatement, parameters);
    if (!executed || executed->rowCount() != 1 ||
        executed->row(0).getInt64(0, -1) != 303 || !executed->row(0).isNull(1)) {
        return 7;
    }
    auto closed = client.closePrepared(kStatement);
    if (!closed) {
        return 8;
    }

    auto failed = client.query("SELECT * FROM galay_postgres_t13_missing_relation");
    if (failed || failed.error().sqlState() != "42P01" || client.transactionStatus() != 'I') {
        return 9;
    }
    auto recovered = client.query("SELECT 404");
    if (!recovered || recovered->row(0).getInt64(0, -1) != 404) {
        return 10;
    }
    client.close();
    return 0;
}
