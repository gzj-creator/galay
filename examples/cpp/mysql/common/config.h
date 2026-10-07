#ifndef GALAY_MYSQL_EXAMPLE_CONFIG_H
#define GALAY_MYSQL_EXAMPLE_CONFIG_H

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace mysql_example
{

struct DbExampleConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user = "root";
    std::string password = "password";
    std::string database = "test";
};

inline const char* get_env_if_set(const char* key)
{
    return std::getenv(key);
}

inline const char* get_env_non_empty(const char* key)
{
    const char* value = get_env_if_set(key);
    if (value == nullptr || value[0] == '\0') {
        return nullptr;
    }
    return value;
}

inline std::string get_env_or_default(const char* key1, const char* key2, const std::string& default_value)
{
    if (const char* value = get_env_if_set(key1)) return value;
    if (const char* value = get_env_if_set(key2)) return value;
    return default_value;
}

inline uint16_t parse_port_or_default(const char* value, uint16_t default_value)
{
    if (value == nullptr || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > 65535UL) {
        return default_value;
    }
    return static_cast<uint16_t>(parsed);
}

inline uint16_t get_env_port_or_default(const char* key1, const char* key2, uint16_t default_value)
{
    if (const char* value = get_env_non_empty(key1)) return parse_port_or_default(value, default_value);
    if (const char* value = get_env_non_empty(key2)) return parse_port_or_default(value, default_value);
    return default_value;
}

inline DbExampleConfig load_db_example_config()
{
    DbExampleConfig cfg;
    cfg.host = get_env_or_default("GALAY_MYSQL_HOST", "MYSQL_HOST", cfg.host);
    cfg.port = get_env_port_or_default("GALAY_MYSQL_PORT", "MYSQL_PORT", cfg.port);
    cfg.user = get_env_or_default("GALAY_MYSQL_USER", "MYSQL_USER", cfg.user);
    cfg.password = get_env_or_default("GALAY_MYSQL_PASSWORD", "MYSQL_PASSWORD", cfg.password);
    cfg.database = get_env_or_default("GALAY_MYSQL_DB", "MYSQL_DATABASE", cfg.database);
    return cfg;
}

inline void print_db_example_config(const DbExampleConfig& cfg)
{
    std::cout << "MySQL config: host=" << cfg.host
              << ", port=" << cfg.port
              << ", user=" << cfg.user
              << ", db=" << cfg.database << std::endl;
}

} // namespace mysql_example

#endif // GALAY_MYSQL_EXAMPLE_CONFIG_H
