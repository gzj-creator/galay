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

struct Required {
    std::string name;
    int age{};
};

#define SERDE_FIELDS_1(X) \
    X(name) \
    X(age)
REFLECT_FIELDS(Required, SERDE_FIELDS_1)
#undef SERDE_FIELDS_1

struct Pair {
    std::array<int, 2> values{};
};

#define SERDE_FIELDS_2(X) \
    X(values)
REFLECT_FIELDS(Pair, SERDE_FIELDS_2)
#undef SERDE_FIELDS_2

struct DateRecord {
    toml::date day;
};

#define SERDE_FIELDS_3(X) \
    X(day)
REFLECT_FIELDS(DateRecord, SERDE_FIELDS_3)
#undef SERDE_FIELDS_3

struct ConstRecord {
    const int value = 1;
};

#define SERDE_FIELDS_4(X) \
    X(value)
REFLECT_FIELDS(ConstRecord, SERDE_FIELDS_4)
#undef SERDE_FIELDS_4

struct WideUnsigned {
    std::uint64_t id{};
};

#define SERDE_FIELDS_5(X) \
    X(id)
REFLECT_FIELDS(WideUnsigned, SERDE_FIELDS_5)
#undef SERDE_FIELDS_5

struct OptionalArray {
    std::vector<std::optional<int>> values;
};

#define SERDE_FIELDS_6(X) \
    X(values)
REFLECT_FIELDS(OptionalArray, SERDE_FIELDS_6)
#undef SERDE_FIELDS_6

struct Scalar {
    int value{};
};

#define SERDE_FIELDS_7(X) \
    X(value)
REFLECT_FIELDS(Scalar, SERDE_FIELDS_7)
#undef SERDE_FIELDS_7

struct Timestamp {
    toml::offset_date_time value;
};

#define SERDE_FIELDS_8(X) \
    X(value)
REFLECT_FIELDS(Timestamp, SERDE_FIELDS_8)
#undef SERDE_FIELDS_8

struct Text {
    std::string value;
};

#define SERDE_FIELDS_9(X) \
    X(value)
REFLECT_FIELDS(Text, SERDE_FIELDS_9)
#undef SERDE_FIELDS_9

struct MultiText {
    std::string value;
};

#define SERDE_FIELDS_10(X) \
    X(value)
REFLECT_FIELDS(MultiText, SERDE_FIELDS_10)
#undef SERDE_FIELDS_10

struct Keyed {
    std::map<std::string, int> values;
};

#define SERDE_FIELDS_11(X) \
    X(values)
REFLECT_FIELDS(Keyed, SERDE_FIELDS_11)
#undef SERDE_FIELDS_11

struct EscapedKey {
    int value{};
};

#define ESCAPED_KEY_FIELDS(X) X(value, "\x01")
REFLECT_FIELDS(EscapedKey, ESCAPED_KEY_FIELDS)
#undef ESCAPED_KEY_FIELDS

/**
 * @brief 检查结果是否失败并包含预期错误片段。
 * @tparam T 结果中的成功值类型。
 * @param result 待检查的 TOML 操作结果。
 * @param expected 预期出现在错误信息中的文本。
 * @param context 当前测试场景描述。
 * @return 结果失败且错误信息包含 `expected` 时返回 `true`。
 */
template <class T>
bool expect_error(const toml::result<T>& result, std::string_view expected,
                  std::string_view context) {
    if (!result && result.error().find(expected) != std::string::npos) {
        return true;
    }
    if (result) {
        std::println("test_errors: {} unexpectedly succeeded", context);
    } else {
        std::println("test_errors: {} returned unexpected error: {}", context, result.error());
    }
    return false;
}

}  // 匿名命名空间

/**
 * @brief 执行错误处理和边界条件测试。
 * @return 全部错误断言通过时返回 0，否则返回 1。
 */
int main() {
    bool passed = true;

    passed &= expect_error(toml::deserialize<Required>("name = \"Ada\"\n"), "age",
                           "missing required field");
    passed &= expect_error(
        toml::deserialize<Required>("name = \"Ada\"\nage = \"old\"\n"),
        "integer", "scalar type mismatch");
    passed &= expect_error(toml::deserialize<Pair>("values = [1]\n"), "wrong TOML array size",
                           "short fixed-size array");
    passed &= expect_error(toml::deserialize<Pair>("values = [1, \"two\"]\n"), "integer",
                           "array element type mismatch");
    passed &= expect_error(
        toml::deserialize<Required>("name = \"Ada\"\nage = 1\nage = 2\n"),
        "duplicate", "duplicate key");
    passed &= expect_error(
        toml::deserialize<Required>("name = \"Ada\"\nage = 1\n[age]\n"),
        "conflicts", "scalar/table conflict");
    passed &= expect_error(
        toml::deserialize<Required>("name = { value = 1, }\nage = 1\n"),
        "trailing comma", "invalid inline table");
    passed &= expect_error(
        toml::deserialize<Required>("name = \"\\uD800\"\nage = 1\n"),
        "Unicode", "invalid Unicode escape");
    passed &= expect_error(toml::deserialize<DateRecord>("day = 2024-02-30\n"),
                           "calendar", "invalid calendar date");
    passed &= expect_error(toml::deserialize<DateRecord>("day = \"2024-02-29\"\n"),
                           "local date", "temporal type mismatch");
    passed &= expect_error(toml::deserialize<Required>("items = 1\n[[items]]\n"),
                           "array-table conflicts", "array-table/scalar conflict");
    passed &= expect_error(toml::deserialize<Required>("name = \"Ada\"\nage = [1\n"),
                           "unterminated", "unterminated array");
    passed &= expect_error(
        toml::deserialize<Required>("name = \"Ada\"\nage = 9223372036854775808\n"),
        "signed 64-bit", "integer outside TOML range");
    passed &= expect_error(toml::deserialize<ConstRecord>("value = 2\n"), "not assignable",
                           "const reflected member");

    passed &= expect_error(toml::serialize(42), "reflected struct/table",
                           "non-reflected serialization root");
    passed &= expect_error(toml::serialize(WideUnsigned{std::numeric_limits<std::uint64_t>::max()}),
                           "signed 64-bit", "wide unsigned integer serialization");
    passed &= expect_error(toml::serialize(OptionalArray{{1, std::nullopt, 3}}), "cannot contain null",
                           "optional null inside TOML array");
    passed &= expect_error(toml::serialize(DateRecord{{2024, 2, 30}}), "invalid toml::date",
                           "invalid temporal serialization value");

    passed &= expect_error(toml::deserialize<Scalar>("value = 01\n"),
                           "leading zero", "leading zero integer");
    passed &= expect_error(toml::deserialize<Scalar>("value = 1.\n"),
                           "fraction", "missing floating fraction");
    passed &= expect_error(toml::deserialize<Scalar>("value = 1e\n"),
                           "exponent", "missing floating exponent");
    passed &= expect_error(toml::deserialize<Scalar>("value =\n1\n"),
                           "missing TOML value", "newline before value");
    passed &= expect_error(toml::deserialize<Scalar>("value = 0x_1\n"),
                           "underscore", "prefixed integer underscore");
    passed &= expect_error(toml::deserialize<Scalar>("value = 1__0\n"),
                           "underscore", "repeated integer underscore");
    passed &= expect_error(toml::deserialize<Scalar>("[section]\na = 1\n[section]\nb = 2\n"),
                           "duplicate", "repeated table header");
    passed &= expect_error(toml::deserialize<Scalar>("value.other = 1\n[value]\nvalue = 2\n"),
                           "duplicate", "dotted key parent cannot be reopened");
    passed &= expect_error(toml::deserialize<Scalar>("value = { nested = 1 }\n[value]\n"),
                           "existing value", "inline table cannot be reopened");
    passed &= expect_error(toml::deserialize<Scalar>("value = 1\n[[value]]\n"),
                           "array-table", "array-table scalar conflict");
    passed &= expect_error(toml::deserialize<Scalar>("value = \"\xFF\"\n"),
                           "UTF-8", "invalid UTF-8 string");
    passed &= expect_error(toml::deserialize<Scalar>("value = \"\x7f\"\n"),
                           "control", "raw DEL control character");
    passed &= expect_error(toml::deserialize<MultiText>("value = \"\"\"bad\x7f\"\"\"\n"),
                           "control", "raw DEL in multiline basic string");
    const auto escaped_text = toml::serialize(Text{"bad\x01"});
    passed &= static_cast<bool>(escaped_text) &&
              escaped_text->find("\\u0001") != std::string::npos;
    const auto escaped_key = toml::serialize(Keyed{{{"bad\x01", 1}}});
    passed &= static_cast<bool>(escaped_key) &&
              escaped_key->find("\\u0001") != std::string::npos;
    const auto escaped_key_input = toml::deserialize<EscapedKey>("\"\\u0001\" = 7\n");
    passed &= escaped_key_input && escaped_key_input->value == 7;

    const toml::ParseOptions limited_input{.max_input_bytes = 4};
    passed &= expect_error(toml::deserialize<Scalar>("value = 1\n", limited_input),
                           "input", "input size limit");

    toml::ParseOptions limited_depth;
    limited_depth.max_depth = 2;
    passed &= expect_error(toml::deserialize<Scalar>("value = [[1]]\n", limited_depth),
                           "depth", "nesting depth limit");

    toml::ParseOptions limited_array;
    limited_array.max_array_items = 2;
    passed &= expect_error(toml::deserialize<OptionalArray>("values = [1, 2, 3]\n", limited_array),
                           "item count", "array item limit");

    toml::ParseOptions strict_schema;
    strict_schema.unknown_fields = toml::UnknownFieldPolicy::reject;
    passed &= expect_error(toml::deserialize<Scalar>("value = 1\nextra = 2\n", strict_schema),
                           "unknown field", "strict unknown field policy");

    toml::SerializeOptions limited_output;
    limited_output.max_output_bytes = 1;
    passed &= expect_error(toml::serialize(Text{"hello"}, limited_output),
                           "output", "output size limit");

    return passed ? 0 : 1;
}
