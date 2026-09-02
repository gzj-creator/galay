#include "common/config.h"

#include <iostream>

import galay.postgres;

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
    auto result = client.query("SELECT current_database(), current_user");
    return result ? 0 : 1;
}
