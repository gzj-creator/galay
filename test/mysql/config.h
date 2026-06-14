#ifndef GALAY_MYSQL_TEST_MYSQL_CONFIG_H
#define GALAY_MYSQL_TEST_MYSQL_CONFIG_H

#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

namespace mysql_test
{

struct MysqlTestConfig {
    std::string host;
    uint16_t port = 3306;
    std::string user;
    std::string password;
    std::string database;
};

inline constexpr int kMysqlTestSkippedExitCode = 125;

inline const char* getEnvNonEmpty(const char* key)
{
    const char* value = std::getenv(key);
    if (value == nullptr || value[0] == '\0') {
        return nullptr;
    }
    return value;
}

inline std::string getEnvOrDefault(const char* key1, const char* key2, const std::string& default_value)
{
    if (const char* value = getEnvNonEmpty(key1)) {
        return value;
    }
    if (const char* value = getEnvNonEmpty(key2)) {
        return value;
    }
    return default_value;
}

inline uint16_t parsePortOrDefault(const char* value, uint16_t default_value)
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

inline uint16_t getEnvPortOrDefault(const char* key1, const char* key2, uint16_t default_value)
{
    if (const char* value = getEnvNonEmpty(key1)) {
        return parsePortOrDefault(value, default_value);
    }
    if (const char* value = getEnvNonEmpty(key2)) {
        return parsePortOrDefault(value, default_value);
    }
    return default_value;
}

inline MysqlTestConfig loadMysqlTestConfig()
{
    MysqlTestConfig cfg;
    cfg.host = getEnvOrDefault("GALAY_MYSQL_HOST", "MYSQL_HOST", cfg.host);
    cfg.port = getEnvPortOrDefault("GALAY_MYSQL_PORT", "MYSQL_PORT", cfg.port);
    cfg.user = getEnvOrDefault("GALAY_MYSQL_USER", "MYSQL_USER", cfg.user);
    cfg.password = getEnvOrDefault("GALAY_MYSQL_PASSWORD", "MYSQL_PASSWORD", cfg.password);
    cfg.database = getEnvOrDefault("GALAY_MYSQL_DB", "MYSQL_DATABASE", cfg.database);
    return cfg;
}

inline bool hasRequiredMysqlTestConfig(const MysqlTestConfig& cfg)
{
    return !cfg.host.empty()
        && !cfg.user.empty()
        && !cfg.database.empty();
}

inline int requireMysqlTestConfigOrSkip(const MysqlTestConfig& cfg, const char* test_name)
{
    if (hasRequiredMysqlTestConfig(cfg)) {
        return 0;
    }

    std::cerr << test_name
              << " skipped: set GALAY_MYSQL_HOST, GALAY_MYSQL_PORT, GALAY_MYSQL_USER, "
                 "GALAY_MYSQL_PASSWORD, GALAY_MYSQL_DB (or MYSQL_* compatibility variables)."
              << std::endl;
    return kMysqlTestSkippedExitCode;
}

inline void printMysqlTestConfig(const MysqlTestConfig& cfg)
{
    std::cout << "MySQL config: host=" << cfg.host
              << ", port=" << cfg.port
              << ", user=" << cfg.user
              << ", db=" << cfg.database << std::endl;
}

} // namespace mysql_test

#endif // GALAY_MYSQL_TEST_MYSQL_CONFIG_H
