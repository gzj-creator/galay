#include "benchmark/cpp/mysql/common/config.h"
#include "examples/cpp/mysql/common/config.h"
#include "config.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

class ScopedEnvOverride {
public:
    ScopedEnvOverride(const char* key, const char* value)
        : m_key(key)
    {
        const char* current = std::getenv(key);
        if (current != nullptr) {
            m_old_value = current;
        }
        if (value == nullptr) {
            unsetenv(key);
        } else {
            setenv(key, value, 1);
        }
    }

    ~ScopedEnvOverride()
    {
        if (m_old_value.has_value()) {
            setenv(m_key.c_str(), m_old_value->c_str(), 1);
        } else {
            unsetenv(m_key.c_str());
        }
    }

private:
    std::string m_key;
    std::optional<std::string> m_old_value;
};

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    ScopedEnvOverride galay_password("GALAY_MYSQL_PASSWORD", "");
    ScopedEnvOverride mysql_password("MYSQL_PASSWORD", nullptr);

    const auto example_cfg = mysql_example::loadDbExampleConfig();
    if (!require(example_cfg.password.empty(),
                 "example config must honor empty GALAY_MYSQL_PASSWORD")) {
        return 1;
    }

    const auto benchmark_cfg = mysql_benchmark::loadDbBenchmarkConfig();
    if (!require(benchmark_cfg.password.empty(),
                 "benchmark config must honor empty GALAY_MYSQL_PASSWORD")) {
        return 1;
    }

    const auto test_cfg = mysql_test::loadDbTestConfig();
    if (!require(test_cfg.password.empty(),
                 "test config must honor empty GALAY_MYSQL_PASSWORD")) {
        return 1;
    }

    std::cout << "T9-ConfigEnvSurface PASS\n";
    return 0;
}
