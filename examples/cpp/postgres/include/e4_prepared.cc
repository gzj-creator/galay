#include "common/config.h"

#include <galay/cpp/galay-postgres/sync/postgres_client.h>

#include <iostream>
#include <optional>
#include <string>
#include <vector>

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
    auto prepared = client.prepare("galay_add", "SELECT $1::int + $2::int");
    if (!prepared) {
        std::cerr << prepared.error().message() << '\n';
        return 1;
    }
    const std::vector<std::optional<std::string>> parameters{std::string("20"), std::string("22")};
    auto result = client.execute("galay_add", parameters);
    if (!result || result->rowCount() == 0) {
        std::cerr << (result ? "empty result" : result.error().message()) << '\n';
        return 1;
    }
    std::cout << "20 + 22 = " << result->row(0).getString(0) << '\n';
    auto closed = client.closePrepared("galay_add");
    return closed ? 0 : 1;
}
