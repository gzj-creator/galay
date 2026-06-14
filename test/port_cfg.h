#ifndef GALAY_TEST_TEST_PORT_CONFIG_H
#define GALAY_TEST_TEST_PORT_CONFIG_H

#include <cstdint>
#include <cstdlib>

namespace galay::test {

inline uint16_t resolvePortFromEnv(const char* env_name, uint16_t default_port) {
    const char* value = std::getenv(env_name);
    if (value == nullptr || *value == '\0') {
        return default_port;
    }

    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > 65535UL) {
        return default_port;
    }

    return static_cast<uint16_t>(parsed);
}

}  // namespace galay::test

#endif
