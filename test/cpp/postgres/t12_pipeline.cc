#include "config.h"

#include <galay/cpp/galay-postgres/protoc/builder.h>
#include <galay/cpp/galay-postgres/sync/postgres_client.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>

namespace
{

using galay::postgres::PostgresClient;
using galay::postgres::protocol::PostgresCommandBuilder;

bool verifySingleValue(const galay::postgres::PostgresResultSet& result, int64_t expected)
{
    return result.rowCount() == 1 && result.fieldCount() == 1 &&
           result.row(0).getInt64(0, -1) == expected;
}

} // namespace

int main()
{
    auto config = galay::postgres::test::integrationConfig();
    if (!config) {
        std::cerr << "t12_pipeline skipped: set GALAY_IT_ENABLE=1 and "
                     "GALAY_POSTGRES_TEST_{HOST,PORT,USER,PASSWORD,DATABASE}.\n";
        return 125;
    }

    PostgresClient client;
    auto connected = client.connect(*config);
    if (!connected) {
        std::cerr << "PostgreSQL connect failed: " << connected.error().message() << '\n';
        return EXIT_FAILURE;
    }

    PostgresCommandBuilder simple_builder;
    simple_builder.appendQuery("SELECT 11").appendQuery("SELECT 22").appendQuery("SELECT 33");
    auto simple_encoding = simple_builder.build();
    if (simple_encoding.expected_ready != 3) {
        std::cerr << "three Query messages must require three ReadyForQuery boundaries\n";
        return EXIT_FAILURE;
    }
    auto simple = client.batch(simple_builder.commands());
    if (!simple || simple->size() != 3 || !verifySingleValue((*simple)[0], 11) ||
        !verifySingleValue((*simple)[1], 22) || !verifySingleValue((*simple)[2], 33)) {
        std::cerr << "simple-query batch boundary/result mismatch";
        if (!simple) {
            std::cerr << ": " << simple.error().message();
        }
        std::cerr << '\n';
        return EXIT_FAILURE;
    }

    constexpr std::string_view kStatement = "galay_t12_extended_batch";
    std::array<std::optional<std::string>, 2> parameters{
        std::string("44"), std::nullopt};
    PostgresCommandBuilder extended_builder;
    extended_builder
        .appendParse(kStatement,
                     "SELECT $1::int4 AS value, $2::text AS nullable_value")
        .appendBind({},
                    kStatement,
                    std::span<const std::optional<std::string>>(parameters))
        .appendDescribePortal({})
        .appendExecute({})
        .appendSync();
    auto extended_encoding = extended_builder.build();
    if (extended_encoding.expected_ready != 1 || extended_builder.size() != 5) {
        std::cerr << "Parse/Bind/Describe/Execute/Sync must have one ReadyForQuery boundary\n";
        return EXIT_FAILURE;
    }

    const auto started = std::chrono::steady_clock::now();
    auto extended = client.batch(extended_builder.commands());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!extended || extended->size() != 1 || extended->front().rowCount() != 1 ||
        extended->front().fieldCount() != 2 ||
        extended->front().row(0).getInt64(0, -1) != 44 ||
        !extended->front().row(0).isNull(1)) {
        std::cerr << "extended-query batch boundary/result mismatch";
        if (!extended) {
            std::cerr << ": " << extended.error().message();
        }
        std::cerr << '\n';
        return EXIT_FAILURE;
    }
    if (elapsed >= std::chrono::seconds(10)) {
        std::cerr << "sync batch waited beyond its single Sync boundary\n";
        return EXIT_FAILURE;
    }

    auto recovered = client.query("SELECT 55");
    if (!recovered || !verifySingleValue(*recovered, 55)) {
        std::cerr << "connection was not aligned after extended batch\n";
        return EXIT_FAILURE;
    }
    auto closed_statement = client.closePrepared(kStatement);
    if (!closed_statement) {
        std::cerr << "prepared statement close failed: "
                  << closed_statement.error().message() << '\n';
        return EXIT_FAILURE;
    }
    client.close();
    return EXIT_SUCCESS;
}
