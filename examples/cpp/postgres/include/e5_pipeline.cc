#include "common/config.h"

#include <galay/cpp/galay-postgres/sync/postgres_client.h>

#include <array>
#include <iostream>
#include <string_view>

int main()
{
    const auto config = postgres_example::loadConfig();
    galay::postgres::PostgresClient client;
    auto connected = client.connect(config.host, config.port, config.user,
                                    config.password, config.database);
    if (!connected) {
        std::cerr << connected.error().message() << '\n';
        return 1;
    }
    constexpr std::array<std::string_view, 3> statements{
        "SELECT 1", "SELECT 2", "SELECT 3"};
    auto results = client.pipeline(statements);
    if (!results || results->size() != statements.size()) {
        std::cerr << (results ? "pipeline result count mismatch" : results.error().message()) << '\n';
        return 1;
    }
    for (const auto& result : *results) {
        std::cout << result.row(0).getString(0) << '\n';
    }
    return 0;
}
