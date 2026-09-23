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

/// 上游 tests/test.toml 的逐字节内嵌副本，测试不依赖工作目录。
constexpr std::string_view galay_serde_test_fixture() {
    return R"serde_toml(# TOML 1.0 语法样例。这里有意混用不同的词法形式，确保解析器能够处理注释、
# 键、字符串、标量、集合、表和数组表，而不只是解析自身序列化生成的文本。

title = "TOML syntax fixture"
bare_key = "bare key"
"quoted key" = "quoted key"
'quoted.dot' = "quoted dotted key"

basic_string = "quote: \" slash: \\ tab: \t unicode: \u03B1 rocket: \U0001F680"
literal_string = 'C:\Users\toml\config.toml'
multiline_basic = """
Roses are red
Violets are blue"""
multiline_continuation = """\
    The quick \
    brown fox"""
multiline_literal = '''
C:\Users\toml\
literal text'''

integer = 1_000
negative_integer = -42
positive_integer = +99
hexadecimal = 0xDEAD_BEEF
octal = 0o755
binary = 0b1101_0010
float_value = 3.141_592_65
scientific = 5e+22
negative_infinity = -inf
not_a_number = nan
enabled = true

local_date = 1979-05-27
local_time = 07:32:00.123456789
local_datetime = 1979-05-27T07:32:00.123
offset_datetime = 1979-05-27T07:32:00.123+07:00
utc_datetime = 1979-05-27 07:32:00Z

numbers = [1, 2, 3]
multiline_array = [
    1,
    # 数组元素之间允许出现注释
    2,
    3,
]
nested_array = [[1, 2], [3, 4]]
inline = { name = "inline table", count = 2, nested.enabled = true }
dotted.parent.child = "dotted key"

[table]
name = "regular table"

[table.nested]
enabled = false
labels = ["one", "two"]

[[products]]
name = "Hammer"
sku = 738594937

[products.dimensions]
width = 3
height = 4

[[products]]
name = "Nail"
sku = 284758393
)serde_toml";
}

}  // namespace


namespace {

struct NestedFlag {
    bool enabled{};

    /**
     * @brief 比较嵌套标志。
     * @param other 待比较的标志。
     * @return 标志值相同时返回 `true`。
     */
    bool operator==(const NestedFlag& other) const = default;
};

#define SERDE_FIELDS_19(X) \
    X(enabled)
REFLECT_FIELDS(NestedFlag, SERDE_FIELDS_19)
#undef SERDE_FIELDS_19

struct InlineFixture {
    std::string name;
    int count{};
    NestedFlag nested;

    /**
     * @brief 比较内联表测试值。
     * @param other 待比较的内联表。
     * @return 所有字段均相同时返回 `true`。
     */
    bool operator==(const InlineFixture& other) const = default;
};

#define SERDE_FIELDS_20(X) \
    X(name) \
    X(count) \
    X(nested)
REFLECT_FIELDS(InlineFixture, SERDE_FIELDS_20)
#undef SERDE_FIELDS_20

struct DottedParent {
    std::string child;

    /**
     * @brief 比较点号键的父对象。
     * @param other 待比较的父对象。
     * @return 子字段相同时返回 `true`。
     */
    bool operator==(const DottedParent& other) const = default;
};

#define SERDE_FIELDS_21(X) \
    X(child)
REFLECT_FIELDS(DottedParent, SERDE_FIELDS_21)
#undef SERDE_FIELDS_21

struct Dotted {
    DottedParent parent;

    /**
     * @brief 比较点号键测试对象。
     * @param other 待比较的对象。
     * @return 父字段相同时返回 `true`。
     */
    bool operator==(const Dotted& other) const = default;
};

#define SERDE_FIELDS_22(X) \
    X(parent)
REFLECT_FIELDS(Dotted, SERDE_FIELDS_22)
#undef SERDE_FIELDS_22

struct NestedTable {
    bool enabled{};
    std::vector<std::string> labels;

    /**
     * @brief 比较嵌套表。
     * @param other 待比较的嵌套表。
     * @return 所有字段均相同时返回 `true`。
     */
    bool operator==(const NestedTable& other) const = default;
};

#define SERDE_FIELDS_23(X) \
    X(enabled) \
    X(labels)
REFLECT_FIELDS(NestedTable, SERDE_FIELDS_23)
#undef SERDE_FIELDS_23

struct RegularTable {
    std::string name;
    NestedTable nested;

    /**
     * @brief 比较普通表。
     * @param other 待比较的普通表。
     * @return 所有字段均相同时返回 `true`。
     */
    bool operator==(const RegularTable& other) const = default;
};

#define SERDE_FIELDS_24(X) \
    X(name) \
    X(nested)
REFLECT_FIELDS(RegularTable, SERDE_FIELDS_24)
#undef SERDE_FIELDS_24

struct Dimensions {
    int width{};
    int height{};

    /**
     * @brief 比较尺寸。
     * @param other 待比较的尺寸。
     * @return 宽高均相同时返回 `true`。
     */
    bool operator==(const Dimensions& other) const = default;
};

#define SERDE_FIELDS_25(X) \
    X(width) \
    X(height)
REFLECT_FIELDS(Dimensions, SERDE_FIELDS_25)
#undef SERDE_FIELDS_25

struct Product {
    std::string name;
    std::int64_t sku{};
    std::optional<Dimensions> dimensions;

    /**
     * @brief 比较产品。
     * @param other 待比较的产品。
     * @return 所有字段均相同时返回 `true`。
     */
    bool operator==(const Product& other) const = default;
};

#define SERDE_FIELDS_26(X) \
    X(name) \
    X(sku) \
    X(dimensions)
REFLECT_FIELDS(Product, SERDE_FIELDS_26)
#undef SERDE_FIELDS_26

struct SyntaxDocument {
    std::string title;
    std::string bare_key;
    std::string quoted_key;
    std::string quoted_dot;
    std::string basic_string;
    std::string literal_string;
    std::string multiline_basic;
    std::string multiline_continuation;
    std::string multiline_literal;
    std::int64_t integer{};
    std::int64_t negative_integer{};
    std::int64_t positive_integer{};
    std::int64_t hexadecimal{};
    std::int64_t octal{};
    std::int64_t binary{};
    double float_value{};
    double scientific{};
    double negative_infinity{};
    double not_a_number{};
    bool enabled{};
    toml::date local_date;
    toml::time local_time;
    toml::local_date_time local_datetime;
    toml::offset_date_time offset_datetime;
    toml::offset_date_time utc_datetime;
    std::vector<int> numbers;
    std::vector<int> multiline_array;
    std::vector<std::vector<int>> nested_array;
    InlineFixture inline_table;
    Dotted dotted;
    RegularTable table;
    std::vector<Product> products;
};

#define SYNTAX_DOCUMENT_FIELDS(X)                  \
    X(title, "title")                              \
    X(bare_key, "bare_key")                        \
    X(quoted_key, "quoted key")                    \
    X(quoted_dot, "quoted.dot")                    \
    X(basic_string, "basic_string")                \
    X(literal_string, "literal_string")            \
    X(multiline_basic, "multiline_basic")          \
    X(multiline_continuation, "multiline_continuation") \
    X(multiline_literal, "multiline_literal")      \
    X(integer, "integer")                          \
    X(negative_integer, "negative_integer")        \
    X(positive_integer, "positive_integer")        \
    X(hexadecimal, "hexadecimal")                  \
    X(octal, "octal")                              \
    X(binary, "binary")                            \
    X(float_value, "float_value")                  \
    X(scientific, "scientific")                    \
    X(negative_infinity, "negative_infinity")      \
    X(not_a_number, "not_a_number")                \
    X(enabled, "enabled")                          \
    X(local_date, "local_date")                    \
    X(local_time, "local_time")                    \
    X(local_datetime, "local_datetime")            \
    X(offset_datetime, "offset_datetime")          \
    X(utc_datetime, "utc_datetime")                \
    X(numbers, "numbers")                          \
    X(multiline_array, "multiline_array")          \
    X(nested_array, "nested_array")                \
    X(inline_table, "inline")                       \
    X(dotted, "dotted")                            \
    X(table, "table")                              \
    X(products, "products")

REFLECT_FIELDS(SyntaxDocument, SYNTAX_DOCUMENT_FIELDS)

#undef SYNTAX_DOCUMENT_FIELDS

/**
 * @brief 检查断言条件并在失败时输出测试信息。
 * @param condition 待检查的条件。
 * @param message 条件失败时显示的信息。
 * @return 原样返回 `condition`。
 */
bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::println("test_parse_fixture: {}", message);
    }
    return condition;
}

}  // 匿名命名空间

/**
 * @brief 读取 TOML 固定样例并验证各类语法。
 * @return 全部断言通过时返回 0，否则返回 1。
 */
int main() {
    const std::string source{galay_serde_test_fixture()};
    const auto parsed = toml::deserialize<SyntaxDocument>(source);
    if (!parsed) {
        std::println("test_parse_fixture: {}", parsed.error());
        return 1;
    }

    const auto& value = *parsed;
    bool passed = true;
    passed &= expect(value.title == "TOML syntax fixture" &&
                         value.bare_key == "bare key" &&
                         value.quoted_key == "quoted key" &&
                         value.quoted_dot == "quoted dotted key",
                     "bare, quoted, and dotted keys parse correctly");
    passed &= expect(value.basic_string == "quote: \" slash: \\ tab: \t unicode: \xce\xb1 rocket: "
                                          "\xf0\x9f\x9a\x80",
                     "basic string escapes parse correctly");
    passed &= expect(value.literal_string == "C:\\Users\\toml\\config.toml",
                     "literal strings parse correctly");
    passed &= expect(value.multiline_basic == "Roses are red\nViolets are blue",
                     "multiline basic strings parse correctly");
    passed &= expect(value.multiline_continuation == "The quick brown fox",
                     "multiline basic line continuation parses correctly");
    passed &= expect(value.multiline_literal == "C:\\Users\\toml\\\nliteral text",
                     "multiline literal strings parse correctly");
    passed &= expect(value.integer == 1000 && value.negative_integer == -42 &&
                         value.positive_integer == 99 && value.hexadecimal == 0xdeadbeef &&
                         value.octal == 0755 && value.binary == 0b11010010 &&
                         value.float_value == 3.14159265 && value.scientific == 5e22 &&
                         std::isinf(value.negative_infinity) && value.negative_infinity < 0 &&
                         std::isnan(value.not_a_number) && value.enabled,
                     "all TOML scalar number and boolean forms parse correctly");
    passed &= expect(value.local_date == toml::date{1979, 5, 27} &&
                         value.local_time == toml::time{7, 32, 0, "123456789"} &&
                         value.local_datetime == toml::local_date_time{
                                                     {1979, 5, 27}, {7, 32, 0, "123"}} &&
                         value.offset_datetime == toml::offset_date_time{
                                                     {{1979, 5, 27}, {7, 32, 0, "123"}}, 420} &&
                         value.utc_datetime == toml::offset_date_time{
                                                   {{1979, 5, 27}, {7, 32, 0, ""}}, 0},
                     "all TOML temporal value forms parse correctly");
    passed &= expect(value.numbers == std::vector<int>{1, 2, 3} &&
                         value.multiline_array == std::vector<int>{1, 2, 3} &&
                         value.nested_array == std::vector<std::vector<int>>{{1, 2}, {3, 4}},
                     "single-line, multiline, and nested arrays parse correctly");
    passed &= expect(value.inline_table == InlineFixture{"inline table", 2, {true}} &&
                         value.dotted == Dotted{{"dotted key"}} &&
                         value.table == RegularTable{"regular table", {false, {"one", "two"}}},
                     "inline tables, dotted keys, and regular tables parse correctly");
    passed &= expect(value.products == std::vector<Product>{
                         {"Hammer", 738594937, Dimensions{3, 4}},
                         {"Nail", 284758393, std::nullopt}},
                     "array-of-tables and nested table sections parse correctly");
    return passed ? 0 : 1;
}
