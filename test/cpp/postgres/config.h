#ifndef GALAY_TEST_POSTGRES_CONFIG_H
#define GALAY_TEST_POSTGRES_CONFIG_H

#include <cstdlib>
#include <optional>
#include <string>

#include <galay/cpp/galay-postgres/base/postgres_config.h>

namespace galay::postgres::test
{

inline std::optional<PostgresConfig> integrationConfig()
{
    const char* enabled = std::getenv("GALAY_IT_ENABLE");
    if (enabled == nullptr || std::string(enabled) != "1") {
        return std::nullopt;
    }

    const char* host = std::getenv("GALAY_POSTGRES_TEST_HOST");
    const char* port = std::getenv("GALAY_POSTGRES_TEST_PORT");
    const char* user = std::getenv("GALAY_POSTGRES_TEST_USER");
    const char* password = std::getenv("GALAY_POSTGRES_TEST_PASSWORD");
    const char* database = std::getenv("GALAY_POSTGRES_TEST_DATABASE");
    if (host == nullptr || port == nullptr || user == nullptr || password == nullptr ||
        database == nullptr) {
        return std::nullopt;
    }

    char* end = nullptr;
    const unsigned long parsed_port = std::strtoul(port, &end, 10);
    if (end == port || *end != '\0' || parsed_port == 0 || parsed_port > 65535) {
        return std::nullopt;
    }
    return PostgresConfig::create(host,
                                  static_cast<uint16_t>(parsed_port),
                                  user,
                                  password,
                                  database);
}

} // namespace galay::postgres::test

#endif // GALAY_TEST_POSTGRES_CONFIG_H
