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

struct Variety {
    std::string name;

    bool operator==(const Variety&) const = default;
};

#define SERDE_FIELDS_27(X) \
    X(name)
REFLECT_FIELDS(Variety, SERDE_FIELDS_27)
#undef SERDE_FIELDS_27

struct Fruit {
    std::string name;
    std::vector<Variety> varieties;

    bool operator==(const Fruit&) const = default;
};

#define SERDE_FIELDS_28(X) \
    X(name) \
    X(varieties)
REFLECT_FIELDS(Fruit, SERDE_FIELDS_28)
#undef SERDE_FIELDS_28

struct Document {
    std::vector<Fruit> fruits;

    bool operator==(const Document&) const = default;
};

#define SERDE_FIELDS_29(X) \
    X(fruits)
REFLECT_FIELDS(Document, SERDE_FIELDS_29)
#undef SERDE_FIELDS_29

struct Mixed {
    std::vector<double> values;
};

#define SERDE_FIELDS_30(X) \
    X(values)
REFLECT_FIELDS(Mixed, SERDE_FIELDS_30)
#undef SERDE_FIELDS_30

struct InlineArray {
    struct nested {
        std::vector<int> values;

        bool operator==(const nested&) const = default;
    };

    nested values;
};

#define SERDE_FIELDS_31(X) \
    X(values)
REFLECT_FIELDS(InlineArray::nested, SERDE_FIELDS_31)
#undef SERDE_FIELDS_31
#define SERDE_FIELDS_32(X) \
    X(values)
REFLECT_FIELDS(InlineArray, SERDE_FIELDS_32)
#undef SERDE_FIELDS_32

struct MultiQuote {
    std::string value;
};

#define SERDE_FIELDS_33(X) \
    X(value)
REFLECT_FIELDS(MultiQuote, SERDE_FIELDS_33)
#undef SERDE_FIELDS_33

struct NestedGrandchild {
    std::string name;

    bool operator==(const NestedGrandchild&) const = default;
};

#define SERDE_FIELDS_34(X) \
    X(name)
REFLECT_FIELDS(NestedGrandchild, SERDE_FIELDS_34)
#undef SERDE_FIELDS_34

struct NestedChild {
    std::vector<NestedGrandchild> grandchildren;

    bool operator==(const NestedChild&) const = default;
};

#define SERDE_FIELDS_35(X) \
    X(grandchildren)
REFLECT_FIELDS(NestedChild, SERDE_FIELDS_35)
#undef SERDE_FIELDS_35

struct NestedRoot {
    NestedChild child;

    bool operator==(const NestedRoot&) const = default;
};

#define SERDE_FIELDS_36(X) \
    X(child)
REFLECT_FIELDS(NestedRoot, SERDE_FIELDS_36)
#undef SERDE_FIELDS_36

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::println("test_toml_edges: {}", message);
    }
    return condition;
}

template <class T>
bool expect_error(const toml::result<T>& parsed, std::string_view error,
                  std::string_view message) {
    if (!parsed && parsed.error() == error) {
        return true;
    }
    std::println("test_toml_edges: {} (got: {})", message,
                 parsed ? "unexpected success" : parsed.error());
    return false;
}

bool test_assignments() {
    using Integers = std::map<std::string, int>;
    using Tables = std::map<std::string, Integers>;
    bool passed = true;
    constexpr std::string_view key_limit = "TOML key exceeds configured size limit";

    const auto bare = toml::deserialize<Integers>(
        "AZaz09_-=1\n0=2\n_=3\n-=4");
    passed &= expect(bare && *bare == Integers{{"AZaz09_-", 1}, {"0", 2},
                                               {"_", 3}, {"-", 4}},
                     "bare keys accept every ASCII character class and end of input");

    for (const auto length : {1, 15, 16, 17, 31, 32, 33, 127, 4096}) {
        const std::string key(length, 'k');
        for (const std::string spaces : {std::string{}, std::string(" \t"),
                                         std::string(33, ' ')}) {
            const std::string input = " \t" + key + spaces + "=\t 7 # comment\r\n";
            toml::ParseOptions limits;
            limits.max_key_bytes = key.size() + spaces.size();
            const auto parsed = toml::deserialize<Integers>(input, limits);
            passed &= expect(parsed && *parsed == Integers{{key, 7}},
                             "bare key byte limit includes trailing whitespace, not indentation");
            --limits.max_key_bytes;
            passed &= expect_error(toml::deserialize<Integers>(input, limits), key_limit,
                                   "bare keys reject one raw byte above the configured limit");
        }
    }

    const auto quoted = toml::deserialize<Integers>(
        "\"a.b=c\" = 1\n'c.d=e' = 2\n\"escaped\\\".=name\" = 3\n\"\"=4\n");
    passed &= expect(quoted && *quoted == Integers{{"a.b=c", 1}, {"c.d=e", 2},
                                                   {"escaped\".=name", 3}, {"", 4}},
                     "quoted keys preserve dots, equals signs, escaped quotes and empty names");
    const Tables expected{{"root", {{"child", 1}, {"dot.=key", 2}, {"other", 3}}}};
    const auto dotted = toml::deserialize<Tables>(
        "root . child = 1\nroot.\"dot.=key\"=2\nroot.other=3\n");
    const auto table = toml::deserialize<Tables>(
        "[root]\nchild=1\n\"dot.=key\"=2\nother=3\n");
    passed &= expect(dotted && table && *dotted == expected && *table == expected,
                     "dotted fallback and bare assignments in the current table agree");

    const auto nested = toml::deserialize<std::map<std::string, std::vector<Integers>>>(
        "rows = [\n # row values\n { value = \t 1 },\n { value=2 },\n]\n");
    passed &= expect(nested && nested->at("rows") ==
                         std::vector<Integers>{{{"value", 1}}, {{"value", 2}}},
                     "nested values retain their whitespace and newline handling");

    const std::pair<std::string_view, std::string_view> malformed[] = {
        {"=1", "empty TOML key"},
        {"a b=1", "invalid bare TOML key: a b"},
        {"a?=1", "invalid bare TOML key: a?"},
        {"a\"b\"=1", "invalid bare TOML key: a\"b\""},
        {"a..b=1", "empty TOML key"},
        {"a . =1", "empty TOML key"},
        {"a#=1", "expected '=' in TOML assignment"},
        {"a\n=1", "expected '=' in TOML assignment"},
        {"a\r=1", "expected '=' in TOML assignment"},
        {"a\t", "expected '=' in TOML assignment"},
        {"\"a.b=c=1", "expected '=' in TOML assignment"},
        {"a=", "missing TOML value"},
        {"a= \t", "missing TOML value"},
        {"a=\n1", "missing TOML value"},
        {"a=\r\n1", "missing TOML value"},
        {"a=# comment", "empty TOML atom"},
        {"a==1", "invalid TOML number"},
        {"a=[1] extra", "unexpected characters after TOML assignment"},
        {"a='x' extra", "unexpected characters after TOML assignment"},
        {"a=1\na=", "missing TOML value"},
        {"a=1\na=[2] extra", "unexpected characters after TOML assignment"},
    };
    for (const auto& [input, error] : malformed) {
        passed &= expect_error(toml::deserialize<Integers>(input), error,
                               "malformed assignments retain their diagnostics");
    }
    for (const std::string_view input : {
             "a=1\na=2", "a=1\n\"a\"=2", "'a'=1\na=2", "a.b=1\na=2",
             "a=1\na.b=2", "a={b=1}\na.b=2", "[parent]\na=1\na=2"}) {
        passed &= expect_error(toml::deserialize<Integers>(input),
                               "duplicate or conflicting TOML key",
                               "bare and fallback assignments reject duplicate or conflicting keys");
    }

    toml::ParseOptions limits;
    limits.max_key_bytes = 7;
    passed &= expect(toml::deserialize<Integers>("\"a.b=c\"=1", limits).has_value(),
                     "quoted keys accept the exact raw key byte limit");
    limits.max_key_bytes = 6;
    passed &= expect_error(toml::deserialize<Integers>("\"a.b=c\"=1", limits), key_limit,
                           "quoted keys reject one raw byte above their limit");
    passed &= expect(toml::deserialize<Tables>("a . b =1", limits).has_value(),
                     "dotted keys include internal and trailing whitespace in their limit");
    limits.max_key_bytes = 5;
    passed &= expect_error(toml::deserialize<Tables>("a . b =1", limits), key_limit,
                           "dotted keys reject one raw byte above their limit");

    limits = {};
    limits.max_depth = 0;
    passed &= expect_error(toml::deserialize<Integers>("a=1", limits), key_limit,
                           "zero depth rejects a bare key before parsing the value");
    limits.max_depth = 1;
    passed &= expect(toml::deserialize<Integers>("a=1", limits).has_value(),
                     "one depth level accepts a bare scalar assignment");
    passed &= expect_error(toml::deserialize<Tables>("a.b=1", limits), key_limit,
                           "dotted keys enforce the component depth limit");
    passed &= expect_error(toml::deserialize<Integers>("a=[1]", limits),
                           "TOML nesting depth exceeds configured limit",
                           "bare assignments still enforce nested value depth");
    limits.max_depth = 2;
    passed &= expect(toml::deserialize<Tables>("a.b=1", limits).has_value(),
                     "dotted keys accept the exact component depth limit");
    limits = {};
    limits.max_nodes = 1;
    passed &= expect_error(toml::deserialize<Integers>("a=1\na=2", limits),
                           "TOML node count exceeds configured limit",
                           "value limits are checked before duplicate insertion");
    return passed;
}

}  // namespace

int main() {
    const auto parsed = toml::deserialize<Document>(
        "[[fruits]]\n"
        "name = \"apple\"\n"
        "[[fruits.varieties]]\n"
        "name = \"red delicious\"\n"
        "[[fruits.varieties]]\n"
        "name = \"granny smith\"\n"
        "[[fruits]]\n"
        "name = \"banana\"\n"
        "[[fruits.varieties]]\n"
        "name = \"cavendish\"\n");

    const Document expected{
        {{"apple", {{"red delicious"}, {"granny smith"}}},
         {"banana", {{"cavendish"}}}}};
    bool passed = expect(parsed && *parsed == expected,
                         "nested array-of-tables preserve parent element context");
    if (parsed) {
        const auto serialized = toml::serialize(*parsed);
        passed &= expect(static_cast<bool>(serialized), "nested array-of-tables serialize");
        if (serialized) {
            const auto reparsed = toml::deserialize<Document>(*serialized);
            passed &= expect(reparsed && *reparsed == expected,
                             "nested array-of-tables round-trip");
        }
    }

    passed &= expect(toml::deserialize<Document>(
                         "[[fruits]]\nname = \"apple\"\n[fruits]\nname = \"bad\"\n")
                         .has_value() == false,
                     "array-table cannot be reopened as a regular table");
    const auto mixed = toml::deserialize<Mixed>("values = [1, 2.0]\n");
    passed &= expect(mixed && mixed->values == std::vector<double>{1.0, 2.0},
                     "mixed TOML arrays remain valid and decode numerically");
    const auto inline_array = toml::deserialize<InlineArray>(
        "values = { values = [\n1,\n2,\n] }\n");
    passed &= expect(inline_array && inline_array->values.values == std::vector<int>{1, 2},
                     "inline tables allow newlines inside nested arrays");
    const auto multi_quote = toml::deserialize<MultiQuote>(
        "value = \"\"\"two \"\" quotes\"\"\"\n");
    passed &= expect(multi_quote && multi_quote->value == "two \"\" quotes",
                     "multiline basic strings preserve four quote runs");

    const auto nested = toml::deserialize<NestedRoot>(
        "[[child.grandchildren]]\nname = \"first\"\n"
        "[[child.grandchildren]]\nname = \"second\"\n");
    passed &= expect(nested && nested->child.grandchildren ==
                         std::vector<NestedGrandchild>{{"first"}, {"second"}},
                     "repeated nested array tables remain appendable");
    passed &= test_assignments();
    return passed ? 0 : 1;
}
