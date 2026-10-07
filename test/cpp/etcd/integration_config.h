#ifndef GALAY_ETCD_TEST_INTEGRATION_CONFIG_H
#define GALAY_ETCD_TEST_INTEGRATION_CONFIG_H

#include <cstdlib>
#include <iostream>
#include <string>

namespace etcd_test
{

inline constexpr int kEtcdTestSkippedExitCode = 125;

inline bool integration_enabled()
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

inline int require_integration_enabled_or_skip(const char* test_name)
{
    if (integration_enabled()) {
        return 0;
    }

    std::cout << "[SKIP] set GALAY_IT_ENABLE=1 to run "
              << test_name << std::endl;
    return kEtcdTestSkippedExitCode;
}

} // namespace etcd_test

#endif // GALAY_ETCD_TEST_INTEGRATION_CONFIG_H
