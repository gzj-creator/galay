#include "common/config.h"

#include <array>
#include <string_view>

import galay.postgres;

int main()
{
    const auto config = postgres_example::loadConfig();
    galay::postgres::PostgresClient client;
    auto connected = client.connect(config.host, config.port, config.user,
                                    config.password, config.database);
    if (!connected) return 1;
    constexpr std::array<std::string_view, 3> sql{"SELECT 1", "SELECT 2", "SELECT 3"};
    auto results = client.pipeline(sql);
    return results && results->size() == sql.size() ? 0 : 1;
}
