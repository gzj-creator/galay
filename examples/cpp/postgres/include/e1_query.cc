#include "common/config.h"

#include <galay/cpp/galay-postgres/sync/postgres_client.h>

#include <iostream>

int main()
{
    const auto config = postgres_example::load_config();
    postgres_example::print_config(config);

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
    if (result->row_count() != 0) {
        std::cout << result->row(0).get_string(0) << " / "
                  << result->row(0).get_string(1) << '\n';
    }
    return 0;
}
