#ifndef GALAY_REDIS_TEST_INTEGRATION_CONFIG_H
#define GALAY_REDIS_TEST_INTEGRATION_CONFIG_H

#include <cstdlib>
#include <iostream>
#include <string>

namespace redis_test
{

inline constexpr int kRedisTestSkippedExitCode = 125;

inline bool integrationEnabled()
{
    const char* value = std::getenv("GALAY_IT_ENABLE");
    if (value == nullptr || value[0] == '\0') {
        return false;
    }

    const std::string enabled(value);
    return enabled == "1"
        || enabled == "true"
        || enabled == "TRUE"
        || enabled == "yes"
        || enabled == "YES"
        || enabled == "on"
        || enabled == "ON";
}

inline int requireIntegrationEnabledOrSkip(const char* test_name)
{
    if (integrationEnabled()) {
        return 0;
    }

    std::cout << "[SKIP] set GALAY_IT_ENABLE=1 to run "
              << test_name << std::endl;
    return kRedisTestSkippedExitCode;
}

} // namespace redis_test

#endif // GALAY_REDIS_TEST_INTEGRATION_CONFIG_H
