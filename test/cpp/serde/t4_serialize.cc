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

struct InlineSettings {
    int count{};
    std::string name;
};

#define SERDE_FIELDS_13(X) \
    X(count) \
    X(name)
REFLECT_FIELDS(InlineSettings, SERDE_FIELDS_13)
#undef SERDE_FIELDS_13

struct Service {
    std::string host;
    int port{};
};

#define SERDE_FIELDS_14(X) \
    X(host) \
    X(port)
REFLECT_FIELDS(Service, SERDE_FIELDS_14)
#undef SERDE_FIELDS_14

struct Product {
    std::string name;
    std::int64_t sku{};
};

#define SERDE_FIELDS_15(X) \
    X(name) \
    X(sku)
REFLECT_FIELDS(Product, SERDE_FIELDS_15)
#undef SERDE_FIELDS_15

struct WriteDocument {
    std::string quoted_key;
    std::vector<int> array;
    std::string basic;
    std::int64_t binary{};
    bool boolean{};
    toml::date date;
    double finite{};
    toml::InlineTable<InlineSettings> inline_settings;
    toml::local_date_time local_datetime;
    toml::time local_time;
    double not_a_number{};
    double negative_infinity{};
    std::vector<std::vector<int>> nested_array;
    toml::offset_date_time offset_datetime;
    Service service;
    std::map<std::string, int> labels;
    std::vector<Product> products;
};

#define WRITE_DOCUMENT_FIELDS(X)                     \
    X(quoted_key, "quoted key")                      \
    X(array, "array")                                \
    X(basic, "basic")                                \
    X(binary, "binary")                              \
    X(boolean, "boolean")                            \
    X(date, "date")                                  \
    X(finite, "finite")                              \
    X(inline_settings, "inline")                     \
    X(local_datetime, "local_datetime")              \
    X(local_time, "local_time")                      \
    X(not_a_number, "not_a_number")                  \
    X(negative_infinity, "negative_infinity")        \
    X(nested_array, "nested_array")                  \
    X(offset_datetime, "offset_datetime")            \
    X(service, "service")                            \
    X(labels, "labels")                              \
    X(products, "products")

REFLECT_FIELDS(WriteDocument, WRITE_DOCUMENT_FIELDS)

#undef WRITE_DOCUMENT_FIELDS

}  // 匿名命名空间

/**
 * @brief 验证序列化结果符合规范化 TOML 文本。
 * @return 生成文本与预期完全一致时返回 0，否则返回 1。
 */
int main() {
    const WriteDocument value{
        "value with \"quotes\" and a newline\n",
        {1, 2, 3},
        "alpha: \xce\xb1",
        0b11010010,
        true,
        {1979, 5, 27},
        3.5,
        {{2, "inline table"}},
        {{1979, 5, 27}, {7, 32, 0, "123"}},
        {7, 32, 0, "123456789"},
        std::numeric_limits<double>::quiet_NaN(),
        -std::numeric_limits<double>::infinity(),
        {{1, 2}, {3, 4}},
        {{{1979, 5, 27}, {7, 32, 0, "123"}}, 420},
        {"localhost", 8080},
        {{"dot.key", 3}, {"plain", 1}},
        {{"Hammer", 738594937}, {"Nail", 284758393}},
    };

    const auto serialized = toml::serialize(value);
    if (!serialized) {
        std::println("test_serialize: {}", serialized.error());
        return 1;
    }

    const std::string expected =
        "array = [1, 2, 3]\n"
        "basic = \"alpha: \xce\xb1\"\n"
        "binary = 210\n"
        "boolean = true\n"
        "date = 1979-05-27\n"
        "finite = 3.5\n"
        "inline = {count = 2, name = \"inline table\"}\n"
        "local_datetime = 1979-05-27T07:32:00.123\n"
        "local_time = 07:32:00.123456789\n"
        "negative_infinity = -inf\n"
        "nested_array = [[1, 2], [3, 4]]\n"
        "not_a_number = nan\n"
        "offset_datetime = 1979-05-27T07:32:00.123+07:00\n"
        "\"quoted key\" = \"value with \\\"quotes\\\" and a newline\\n\"\n"
        "\n"
        "[labels]\n"
        "\"dot.key\" = 3\n"
        "plain = 1\n"
        "\n"
        "[service]\n"
        "host = \"localhost\"\n"
        "port = 8080\n"
        "\n"
        "[[products]]\n"
        "name = \"Hammer\"\n"
        "sku = 738594937\n"
        "\n"
        "[[products]]\n"
        "name = \"Nail\"\n"
        "sku = 284758393\n";

    if (*serialized != expected) {
        std::println("test_serialize: generated TOML differs from the canonical expected output");
        std::println("generated:\n{}", *serialized);
        return 1;
    }
    return 0;
}
