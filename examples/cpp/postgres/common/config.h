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

inline std::string environment_or(const char* name, std::string fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : fallback;
}

inline DbConfig load_config()
{
    DbConfig config;
    config.host = environment_or("GALAY_POSTGRES_HOST", config.host);
    config.user = environment_or("GALAY_POSTGRES_USER", config.user);
    config.password = environment_or("GALAY_POSTGRES_PASSWORD", config.password);
    config.database = environment_or("GALAY_POSTGRES_DB", config.database);
    const std::string port_text = environment_or("GALAY_POSTGRES_PORT", "5432");
    uint16_t port = 0;
    const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec == std::errc{} && parsed.ptr == port_text.data() + port_text.size() && port != 0) {
        config.port = port;
    }
    return config;
}

inline void print_config(const DbConfig& config)
{
    std::cout << "PostgreSQL: host=" << config.host << ", port=" << config.port
              << ", user=" << config.user << ", database=" << config.database << '\n';
}

} // namespace postgres_example

#endif // GALAY_POSTGRES_EXAMPLE_CONFIG_H
