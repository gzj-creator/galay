#ifndef GALAY_MYSQL_TEST_MYSQL_CONFIG_H
#define GALAY_MYSQL_TEST_MYSQL_CONFIG_H

#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

namespace mysql_test
{

struct DbTestConfig {
    std::string host;
    uint16_t port = 3306;
    std::string user;
    std::string password;
    std::string database;
};

inline constexpr int kMysqlTestSkippedExitCode = 125;

inline const char* get_env_non_empty(const char* key)
{
    const char* value = std::getenv(key);
    if (value == nullptr || value[0] == '\0') {
        return nullptr;
    }
    return value;
}

inline std::string get_env_or_default(const char* key1, const char* key2, const std::string& default_value)
{
    if (const char* value = get_env_non_empty(key1)) {
        return value;
    }
    if (const char* value = get_env_non_empty(key2)) {
        return value;
    }
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
    if (const char* value = get_env_non_empty(key1)) {
        return parse_port_or_default(value, default_value);
    }
    if (const char* value = get_env_non_empty(key2)) {
        return parse_port_or_default(value, default_value);
    }
    return default_value;
}

inline DbTestConfig load_db_test_config()
{
    DbTestConfig cfg;
    cfg.host = get_env_or_default("GALAY_MYSQL_HOST", "MYSQL_HOST", cfg.host);
    cfg.port = get_env_port_or_default("GALAY_MYSQL_PORT", "MYSQL_PORT", cfg.port);
    cfg.user = get_env_or_default("GALAY_MYSQL_USER", "MYSQL_USER", cfg.user);
    cfg.password = get_env_or_default("GALAY_MYSQL_PASSWORD", "MYSQL_PASSWORD", cfg.password);
    cfg.database = get_env_or_default("GALAY_MYSQL_DB", "MYSQL_DATABASE", cfg.database);
    return cfg;
}

inline bool has_required_db_test_config(const DbTestConfig& cfg)
{
    return !cfg.host.empty()
        && !cfg.user.empty()
        && !cfg.database.empty();
}

inline bool is_integration_enabled()
{
    const char* value = get_env_non_empty("GALAY_IT_ENABLE");
    if (value == nullptr) {
        return false;
    }

    const std::string enabled_value(value);
    return enabled_value == "1"
        || enabled_value == "true"
        || enabled_value == "TRUE"
        || enabled_value == "yes"
        || enabled_value == "YES";
}

inline int require_integration_enabled_or_skip(const char* test_name)
{
    if (is_integration_enabled()) {
        return 0;
    }

    std::cerr << test_name
              << " skipped: set GALAY_IT_ENABLE=1 to run external MySQL integration tests."
              << std::endl;
    return kMysqlTestSkippedExitCode;
}

inline int require_db_test_config_or_skip(const DbTestConfig& cfg, const char* test_name)
{
    if (has_required_db_test_config(cfg)) {
        return 0;
    }

    std::cerr << test_name
              << " skipped: set GALAY_MYSQL_HOST, GALAY_MYSQL_PORT, GALAY_MYSQL_USER, "
                 "GALAY_MYSQL_PASSWORD, GALAY_MYSQL_DB (or MYSQL_* compatibility variables)."
              << std::endl;
    return kMysqlTestSkippedExitCode;
}

inline void print_db_test_config(const DbTestConfig& cfg)
{
    std::cout << "MySQL config: host=" << cfg.host
              << ", port=" << cfg.port
              << ", user=" << cfg.user
              << ", db=" << cfg.database << std::endl;
}

} // namespace mysql_test

#endif // GALAY_MYSQL_TEST_MYSQL_CONFIG_H
