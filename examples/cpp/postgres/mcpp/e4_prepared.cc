#include "common/config.h"

#include <optional>
#include <string>
#include <vector>

import galay.postgres;

int main()
{
    const auto config = postgres_example::loadConfig();
    galay::postgres::PostgresClient client;
    auto connected = client.connect(config.host, config.port, config.user,
                                    config.password, config.database);
    if (!connected) return 1;
    auto prepared = client.prepare("galay_add", "SELECT $1::int + $2::int");
    if (!prepared) return 1;
    const std::vector<std::optional<std::string>> params{"20", "22"};
    return client.execute("galay_add", params) ? 0 : 1;
}
