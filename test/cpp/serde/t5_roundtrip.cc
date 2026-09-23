#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <serde/toml/toml.hpp>

#include <serde/reflect/reflect_macros.hpp>

namespace {

enum class Priority : std::uint8_t {
    low = 1,
    high = 2,
};

struct Credentials {
    std::string user;
    std::string token;

    /**
     * @brief 比较凭据内容。
     * @param other 待比较的凭据。
     * @return 用户名和令牌均相同时返回 `true`。
     */
    bool operator==(const Credentials& other) const = default;
};

#define SERDE_FIELDS_16(X) \
    X(user) \
    X(token)
REFLECT_FIELDS(Credentials, SERDE_FIELDS_16)
#undef SERDE_FIELDS_16

struct Endpoint {
    std::string host;
    int port{};
    std::optional<Credentials> credentials;

    /**
     * @brief 比较端点配置。
     * @param other 待比较的端点。
     * @return 所有字段均相同时返回 `true`。
     */
    bool operator==(const Endpoint& other) const = default;
};

#define SERDE_FIELDS_17(X) \
    X(host) \
    X(port) \
    X(credentials)
REFLECT_FIELDS(Endpoint, SERDE_FIELDS_17)
#undef SERDE_FIELDS_17

struct Plugin {
    std::string name;
    bool enabled{};
    std::vector<int> ports;

    /**
     * @brief 比较插件配置。
     * @param other 待比较的插件。
     * @return 所有字段均相同时返回 `true`。
     */
    bool operator==(const Plugin& other) const = default;
};

#define SERDE_FIELDS_18(X) \
    X(name) \
    X(enabled) \
    X(ports)
REFLECT_FIELDS(Plugin, SERDE_FIELDS_18)
#undef SERDE_FIELDS_18

struct Settings {
    std::string application;
    bool enabled{};
    std::int64_t minimum{};
    std::uint32_t retries{};
    double ratio{};
    Priority priority{Priority::low};
    toml::date release_date;
    toml::time start_time;
    toml::offset_date_time updated_at;
    std::optional<std::string> nickname;
    std::array<std::string, 2> labels{};
    std::vector<bool> flags;
    std::vector<std::vector<int>> matrix;
    std::map<std::string, int> thresholds;
    std::unordered_map<std::string, bool> switches;
    toml::InlineTable<Credentials> inline_credentials;
    Endpoint endpoint;
    std::vector<Plugin> plugins;

    /**
     * @brief 比较完整设置。
     * @param other 待比较的设置。
     * @return 所有字段均相同时返回 `true`。
     */
    bool operator==(const Settings& other) const = default;
};

#define SETTINGS_FIELDS(X)       \
    X(application)               \
    X(enabled)                   \
    X(minimum)                   \
    X(retries)                   \
    X(ratio)                     \
    X(priority)                  \
    X(release_date)              \
    X(start_time)                \
    X(updated_at)                \
    X(nickname)                  \
    X(labels)                    \
    X(flags)                     \
    X(matrix)                    \
    X(thresholds)                \
    X(switches)                  \
    X(inline_credentials)        \
    X(endpoint)                  \
    X(plugins)

REFLECT_FIELDS(Settings, SETTINGS_FIELDS)

#undef SETTINGS_FIELDS

static_assert(std::same_as<decltype(toml::serialize(std::declval<const Settings&>())),
                           std::expected<std::string, std::string>>);
static_assert(std::same_as<decltype(toml::deserialize<Settings>(std::string_view{})),
                           std::expected<Settings, std::string>>);

/**
 * @brief 检查断言条件并在失败时输出测试信息。
 * @param condition 待检查的条件。
 * @param message 条件失败时显示的信息。
 * @return 原样返回 `condition`。
 */
bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::println("test_roundtrip: {}", message);
    }
    return condition;
}

}  // 匿名命名空间

/**
 * @brief 执行序列化与反序列化往返测试。
 * @return 全部断言通过时返回 0，否则返回 1。
 */
int main() {
    const Settings original{
        "reflect-toml",
        true,
        std::numeric_limits<std::int64_t>::min(),
        42,
        0.125,
        Priority::high,
        {2026, 9, 4},
        {9, 30, 15, "987654321"},
        {{{2026, 9, 4}, {9, 30, 15, "123"}}, -480},
        std::nullopt,
        {"stable", "portable"},
        {true, false, true},
        {{1, 2}, {3, 4}},
        {{"warning", 70}, {"critical", 90}},
        {{"cache", true}, {"verbose", false}},
        {{"inline-user", "inline-token"}},
        {"127.0.0.1", 8080, Credentials{"admin", "secret"}},
        {{"formatter", true, {9000}}, {"linter", false, {9001, 9002}}},
    };

    const auto serialized = toml::serialize(original);
    if (!serialized) {
        std::println("test_roundtrip: serialize failed: {}", serialized.error());
        return 1;
    }

    const auto canonical = toml::deserialize<Settings>(*serialized);

    bool passed = true;
    passed &= expect(canonical && *canonical == original,
                     "serialize and deserialize preserve every supported field");
    return passed ? 0 : 1;
}
