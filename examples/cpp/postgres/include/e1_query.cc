#include "common/config.h"

#include <galay/cpp/galay-postgres/sync/postgres_client.h>

#include <iostream>

int main()
{
    const auto config = postgres_example::loadConfig();
    postgres_example::printConfig(config);

    galay::postgres::PostgresClient client;
    auto connected = client.connect(config.host,
                                    config.port,
                                    config.user,
                                    config.password,
                                    config.database);
    if (!connected) {
        std::cerr << "connect failed: " << connected.error().message() << '\n';
        return 1;
    }

    auto result = client.query("SELECT current_database(), current_user");
    if (!result) {
        std::cerr << "query failed: " << result.error().message() << '\n';
        return 1;
    }
    if (result->rowCount() != 0) {
        std::cout << result->row(0).getString(0) << " / "
                  << result->row(0).getString(1) << '\n';
    }
    return 0;
}
