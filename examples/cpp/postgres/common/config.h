#ifndef GALAY_POSTGRES_EXAMPLE_CONFIG_H
#define GALAY_POSTGRES_EXAMPLE_CONFIG_H

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <string>

namespace postgres_example
{

struct DbConfig
{
    std::string host = "127.0.0.1";
    std::string user = "postgres";
    std::string password = "postgres";
    std::string database = "postgres";
    uint16_t port = 5432;
};

inline std::string environmentOr(const char* name, std::string fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : fallback;
}

inline DbConfig loadConfig()
{
    DbConfig config;
    config.host = environmentOr("GALAY_POSTGRES_HOST", config.host);
    config.user = environmentOr("GALAY_POSTGRES_USER", config.user);
    config.password = environmentOr("GALAY_POSTGRES_PASSWORD", config.password);
    config.database = environmentOr("GALAY_POSTGRES_DB", config.database);
    const std::string port_text = environmentOr("GALAY_POSTGRES_PORT", "5432");
    uint16_t port = 0;
    const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec == std::errc{} && parsed.ptr == port_text.data() + port_text.size() && port != 0) {
        config.port = port;
    }
    return config;
}

inline void printConfig(const DbConfig& config)
{
    std::cout << "PostgreSQL: host=" << config.host << ", port=" << config.port
              << ", user=" << config.user << ", database=" << config.database << '\n';
}

} // namespace postgres_example

#endif // GALAY_POSTGRES_EXAMPLE_CONFIG_H
