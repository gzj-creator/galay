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
#include <serde/json/json.hpp>
#include <serde/toml/toml.hpp>

#include <serde/reflect/reflect_macros.hpp>

namespace {

struct TextRecord {
    std::string value;

    bool operator==(const TextRecord&) const = default;
};

#define TEXT_RECORD_FIELDS(X) X(value)
REFLECT_FIELDS(TextRecord, TEXT_RECORD_FIELDS)
#undef TEXT_RECORD_FIELDS

struct ScalarRecord {
    int value{};

    bool operator==(const ScalarRecord&) const = default;
};

#define SCALAR_RECORD_FIELDS(X) X(value)
REFLECT_FIELDS(ScalarRecord, SCALAR_RECORD_FIELDS)
#undef SCALAR_RECORD_FIELDS

struct ValuesRecord {
    std::vector<int> values;

    bool operator==(const ValuesRecord&) const = default;
};

#define VALUES_RECORD_FIELDS(X) X(values)
REFLECT_FIELDS(ValuesRecord, VALUES_RECORD_FIELDS)
#undef VALUES_RECORD_FIELDS

struct TemporalRecord {
    toml::date day;
    toml::time clock;
    toml::offset_date_time instant;

    bool operator==(const TemporalRecord&) const = default;
};

#define TEMPORAL_RECORD_FIELDS(X) X(day) X(clock) X(instant)
REFLECT_FIELDS(TemporalRecord, TEMPORAL_RECORD_FIELDS)
#undef TEMPORAL_RECORD_FIELDS

bool expect(bool condition, std::string_view name) {
    if (!condition) {
        std::println("test_parser_boundaries: {}", name);
    }
    return condition;
}

template <class Result>
bool expect_error(const Result& result, std::string_view fragment,
                  std::string_view name) {
    if (!result && result.error().find(fragment) != std::string::npos) {
        return true;
    }
    std::println("test_parser_boundaries: {}: {}", name,
                 result ? "unexpected success" : result.error());
    return false;
}

bool equal_json(const json::Json& left, const json::Json& right) {
    if (left.type() != right.type()) return false;
    switch (left.type()) {
        case json::ValueType::null_value:
            return true;
        case json::ValueType::boolean:
            return *left.as_bool() == *right.as_bool();
        case json::ValueType::signed_integer:
            return *left.as_int64() == *right.as_int64();
        case json::ValueType::unsigned_integer:
            return *left.as_uint64() == *right.as_uint64();
        case json::ValueType::number: {
            const auto left_value = *left.as_double();
            const auto right_value = *right.as_double();
            return left_value == right_value && std::signbit(left_value) == std::signbit(right_value);
        }
        case json::ValueType::string:
            return *left.as_string() == *right.as_string();
        case json::ValueType::array: {
            if (left.size() != right.size()) return false;
            for (std::size_t index = 0; index < left.size(); ++index) {
                if (!equal_json(left.at(index), right.at(index))) return false;
            }
            return true;
        }
        case json::ValueType::object: {
            std::map<std::string_view, char> seen;
            bool equal = true;
            left.for_each_member([&](std::string_view key, const json::Json& child) -> json::result<void> {
                if (!seen.emplace(key, 0).second) return {};
                const auto other = right.at(key);
                if (!other.valid() || !equal_json(child, other)) {
                    equal = false;
                    return std::unexpected(std::string("mismatch"));
                }
                return {};
            });
            if (!equal) return false;
            right.for_each_member([&](std::string_view key, const json::Json&) -> json::result<void> {
                if (!seen.contains(key)) equal = false;
                return {};
            });
            return equal;
        }
        default:
            return false;
    }
}

bool check_json_context(std::string_view text, json::Parser& parser,
                        const json::ParseOptions& options = {}) {
    const auto reference = json::parse(text, options);
    auto agrees = [&](const auto& parsed) {
        if (reference.has_value() != parsed.has_value() ||
            (reference && parsed && !equal_json(*reference, *parsed))) {
            std::println("test_parser_boundaries: JSON context mismatch for {} bytes: {} / {}",
                         text.size(), reference ? "success" : reference.error(),
                         parsed ? "success" : parsed.error());
            return false;
        }
        return true;
    };
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        if (!agrees(parser.parse(text, options))) return false;
    }
    json::Parser fresh;
    return agrees(fresh.parse(text, options));
}

bool check_json_context_boundaries() {
    json::Parser context;
    const std::vector<std::string> corpus{
        "", " \t\r\n", "null", "true", "false", "nul", "truefalse", "0x1", "1 2",
        "0", "-0.0", "1.25e-2", "-", "01", "-01", "1.", "1e+", "1e400",
        "-9223372036854775808", "18446744073709551615", "18446744073709551616",
        "[]", "{}", "[[],{},null,true,false,1,2.5]", "[", "{", "[1,]", "{\"x\":1,}",
        R"({"x":1,"x":2})", R"({"x":1,"\u0078":2})", R"({"x":1,"x":[1,]})",
        R"({"x":1,"x":"\uD800"})", R"({"x":{"b":1,"a":2},"y":[3,4]})",
        R"({"y":[5],"x":{"c":6}})", R"({"x":null})", R"({"x":[]})", R"({"x":{}})",
        R"({"long key that exceeds small string storage":{"different long key":[1,2]}})",
        R"("")", R"("\b\f\n\r\t\/\\\"")", R"("\u0000\u03b1\uD83D\uDE80")",
        R"("\uD800")", R"("\uDC00")", R"("\uD800\u0000")", R"("\uD800\uZZZZ")",
        R"("\u12")", R"("\x")", "\"unterminated\\", "\"line\nbreak\"",
        std::string("\"") + "\xC0\x80" + "\"", std::string("\"") + "\xED\xA0\x80" + "\"",
        std::string("\"") + "\xF4\x90\x80\x80" + "\"", std::string("\"") + "\xF0\x9F" + "\""
    };
    bool passed = true;
    for (const auto& input : corpus) passed &= check_json_context(input, context);
    for (const auto member : {&json::ParseOptions::max_input_bytes,
                              &json::ParseOptions::max_nodes,
                              &json::ParseOptions::max_depth,
                              &json::ParseOptions::max_string_bytes,
                              &json::ParseOptions::max_key_bytes,
                              &json::ParseOptions::max_array_items,
                              &json::ParseOptions::max_object_members}) {
        for (const std::size_t limit : {0U, 1U, 2U, 3U, 16U}) {
            json::ParseOptions options;
            options.*member = limit;
            for (const auto& input : corpus) passed &= check_json_context(input, context, options);
        }
    }
    for (std::size_t prefix = 0; prefix < 130; ++prefix) {
        for (std::size_t slashes = 0; slashes < 6; ++slashes) {
            const auto text = "\"" + std::string(prefix, 'a') + std::string(slashes, '\\') +
                              "\"[],:{}\t";
            passed &= check_json_context(text, context);
        }
        passed &= check_json_context("\"" + std::string(prefix, 'a') +
                                         "\xF0\x9F\x9A\x80\\n\"", context);
    }
    passed &= check_json_context("\"" + std::string(8193, 'a') + "\\n\"", context);
    const std::string seed = R"({"long key":[0,-1,1.5,true,false,null,"a\\\"b","\uD83D\uDE80"],"b":{}})";
    const std::string mutations = "\"\\[]{}:, \t\r\n0-efntx";
    for (std::size_t index = 0; index < seed.size(); ++index) {
        passed &= check_json_context(std::string_view(seed).substr(0, index), context);
        for (const char character : mutations) {
            auto input = seed;
            input[index] = character;
            passed &= check_json_context(input, context);
        }
    }
    return passed;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= check_json_context_boundaries();
    const std::string long_ascii(16 * 257 + 7, 'a');
    const std::string rocket{"\xF0\x9F\x9A\x80"};

    const std::string json_source =
        "{\"value\":\"" + long_ascii + "\\n" + rocket + "\\u03b1\"}";
    const auto json_long = json::deserialize<TextRecord>(json_source);
    passed &= expect(json_long && json_long->value == long_ascii + "\n" + rocket + "\xCE\xB1",
                     "JSON long basic string across SIMD block boundaries");
    const std::string json_one_block = "\"" + std::string(14, 'a') + "\"";
    const auto json_exact_block = json::deserialize<std::string>(json_one_block);
    passed &= expect(json_exact_block &&
                         *json_exact_block == std::string(14, 'a'),
                     "JSON validates an ASCII input exactly one SIMD block long");


    const auto json_escapes = json::deserialize<std::string>(
        R"json("\b\f\n\r\t\/\\\"")json");
    passed &= expect(json_escapes && *json_escapes == "\b\f\n\r\t/\\\"",
                     "JSON supports every short string escape");
    passed &= expect_error(json::deserialize<TextRecord>("{\"value\":\"line\nbreak\"}"),
                           "control", "JSON rejects raw control characters in strings");
    passed &= expect_error(json::deserialize<std::string>(R"json("\uDC00")json"),
                           "low surrogate", "JSON rejects an unpaired low surrogate");

    std::string invalid_json = "{\"value\":\"";
    invalid_json += "\xC0\x80";
    invalid_json += "\"}";
    passed &= expect_error(json::deserialize<TextRecord>(invalid_json), "UTF-8",
                           "JSON rejects overlong UTF-8");
    passed &= expect(json::deserialize<std::int32_t>("2147483647").has_value(),
                     "JSON accepts the destination integer maximum");
    passed &= expect_error(json::deserialize<std::int32_t>("2147483648"), "range",
                           "JSON rejects a destination integer overflow");
    passed &= expect_error(json::deserialize<double>("1."), "fraction",
                           "JSON rejects a number without fractional digits");
    passed &= expect_error(json::deserialize<double>("1e+"), "exponent",
                           "JSON rejects a number without exponent digits");
    passed &= expect_error(json::deserialize<json::time>(R"json("24:00:00")json"),
                           "time", "JSON rejects an out-of-range time");
    passed &= expect_error(
        json::deserialize<json::offset_date_time>(R"json("2024-01-01T00:00:00+24:00")json"),
        "offset", "JSON rejects an out-of-range UTC offset");

    json::Parser reusable_json;
    const auto reusable_first = reusable_json.parse(
        R"json({"alpha":[1,"one"],"nested":{"value":true}})json");
    passed &= expect(reusable_first.has_value(),
                     "JSON reusable parser parses the first document");
    const auto reusable_second = reusable_json.parse(
        R"json({"beta":[2,"two"],"nested":{"other":false}})json");
    passed &= expect(reusable_second.has_value(),
                     "JSON reusable parser resets between documents");
    if (reusable_second) {
        passed &= expect(reusable_second->contains("beta") &&
                             reusable_second->contains("nested") &&
                             !reusable_second->contains("alpha"),
                         "JSON reusable parser does not retain stale object keys");
    }
    passed &= expect(reusable_first && !reusable_first->valid(),
                     "JSON reusable parser invalidates the previous document");
    const auto reusable_failure = reusable_json.parse(R"json({"broken":[1,]})json");
    passed &= expect(!reusable_failure,
                     "JSON reusable parser preserves parse failures");
    passed &= expect(reusable_second && !reusable_second->valid(),
                     "JSON reusable parser invalidates documents after a failed parse");
    const auto reusable_after_failure = reusable_json.parse(R"json({"recovered":3})json");
    passed &= expect(reusable_after_failure.has_value(),
                     "JSON reusable parser recovers after a failed parse");
    reusable_json.reset();
    passed &= expect(reusable_after_failure && !reusable_after_failure->valid(),
                     "JSON Parser::reset invalidates the current document");
    const auto reusable_after_reset = reusable_json.parse(R"json({"reset":true})json");
    passed &= expect(reusable_after_reset.has_value() && reusable_after_reset->contains("reset"),
                     "JSON Parser::reset can parse again");
    json::reset_thread_parser();
    passed &= expect(json::deserialize<int>("7").has_value(),
                     "JSON thread-local parser reset still deserializes");

    json::ParseOptions json_limits;
    json_limits.max_string_bytes = long_ascii.size();
    const auto json_exact_string = json::deserialize<TextRecord>(
        "{\"value\":\"" + long_ascii + "\"}", json_limits);
    passed &= expect(json_exact_string && json_exact_string->value == long_ascii,
                     "JSON accepts a string exactly at its configured limit");
    json_limits.max_string_bytes = long_ascii.size() - 1;
    passed &= expect_error(json::deserialize<TextRecord>(
                               "{\"value\":\"" + long_ascii + "\"}", json_limits),
                           "string", "JSON rejects a string one byte above its configured limit");

    json_limits = {};
    json_limits.max_key_bytes = 4;
    passed &= expect_error(json::deserialize<std::map<std::string, int>>("{\"wide!\":1}", json_limits),
                           "key", "JSON enforces the object-key byte limit");
    json_limits = {};
    json_limits.max_nodes = 3;
    passed &= expect(json::deserialize<std::vector<int>>("[1,2]", json_limits).has_value(),
                     "JSON accepts exactly the configured node count");
    json_limits.max_nodes = 2;
    passed &= expect_error(json::deserialize<std::vector<int>>("[1,2]", json_limits), "node",
                           "JSON rejects one node above the configured limit");
    json_limits = {};
    json_limits.max_object_members = 1;
    passed &= expect_error(json::deserialize<std::map<std::string, int>>("{\"a\":1,\"b\":2}", json_limits),
                           "member", "JSON enforces the object-member limit");
    json_limits = {};
    json_limits.max_array_items = 2;
    passed &= expect(json::deserialize<std::vector<int>>("[1,2]", json_limits).has_value(),
                     "JSON accepts exactly the configured array item limit");
    json_limits = {};
    json_limits.max_object_members = 1;
    passed &= expect(json::deserialize<std::map<std::string, int>>("{\"a\":1}", json_limits).has_value(),
                     "JSON accepts exactly the configured object-member limit");
    const std::string json_limit_input = "{\"value\":1}";
    json_limits = {};
    json_limits.max_input_bytes = json_limit_input.size();
    passed &= expect(json::deserialize<ScalarRecord>(json_limit_input, json_limits).has_value(),
                     "JSON accepts an input exactly at the configured byte limit");
    json_limits.max_input_bytes = json_limit_input.size() - 1;
    passed &= expect_error(json::deserialize<ScalarRecord>(json_limit_input, json_limits),
                           "input", "JSON rejects an input one byte above the configured limit");


    const std::string toml_basic =
        "value = \"" + long_ascii + "\\n\\U0001F680\"\n";
    const auto toml_long = toml::deserialize<TextRecord>(toml_basic);
    passed &= expect(toml_long && toml_long->value == long_ascii + "\n" + rocket,
                     "TOML long basic string across SIMD block boundaries");
    const auto toml_literal = toml::deserialize<TextRecord>(
        "value = '" + long_ascii + "'\n");
    passed &= expect(toml_literal && toml_literal->value == long_ascii,
                     "TOML long literal string across SIMD block boundaries");
    const std::string toml_one_block = "value = \"" + std::string(6, 'a') + "\"";
    const auto toml_exact_block = toml::deserialize<TextRecord>(toml_one_block);
    passed &= expect(toml_exact_block &&
                         toml_exact_block->value == std::string(6, 'a'),
                     "TOML validates an ASCII document exactly one SIMD block long");

    passed &= expect(toml::deserialize<TextRecord>("value = \"# value\" # comment\r\n").has_value(),
                     "TOML accepts CRLF and preserves a quoted comment marker");
    passed &= expect_error(toml::deserialize<TextRecord>("value = \"\\U0000D800\"\n"),
                           "Unicode", "TOML rejects a surrogate Unicode escape");
    passed &= expect_error(toml::deserialize<TextRecord>("value = \"line\nbreak\"\n"),
                           "newline", "TOML rejects a newline in a basic string");
    passed &= expect(toml::deserialize<ScalarRecord>("value = +0\n").has_value() &&
                         toml::deserialize<ScalarRecord>("value = -0\n")->value == 0,
                     "TOML accepts signed zero");
    passed &= expect_error(toml::deserialize<ScalarRecord>("value = 0x1_\n"),
                           "underscore", "TOML rejects a trailing numeric separator");

    const auto valid_temporal = toml::deserialize<TemporalRecord>(
        "day = 2000-02-29\nclock = 23:59:59.999\ninstant = 2024-01-01T00:00:00+23:59\n");
    passed &= expect(valid_temporal.has_value(), "TOML accepts temporal upper bounds");
    passed &= expect_error(toml::deserialize<ScalarRecord>("value = 1900-02-29\n"),
                           "calendar", "TOML rejects a non-leap-century date");
    passed &= expect_error(toml::deserialize<ScalarRecord>("value = 24:00:00\n"),
                           "time", "TOML rejects an out-of-range time");
    passed &= expect_error(toml::deserialize<ScalarRecord>("value = 2024-01-01T00:00:00+24:00\n"),
                           "offset", "TOML rejects an out-of-range UTC offset");

    toml::ParseOptions toml_limits;
    toml_limits.max_string_bytes = long_ascii.size();
    const auto toml_exact_string = toml::deserialize<TextRecord>(
        "value = \"" + long_ascii + "\"\n", toml_limits);
    passed &= expect(toml_exact_string && toml_exact_string->value == long_ascii,
                     "TOML accepts a string exactly at its configured limit");
    toml_limits.max_string_bytes = long_ascii.size() - 1;
    passed &= expect_error(toml::deserialize<TextRecord>(
                               "value = \"" + long_ascii + "\"\n", toml_limits),
                           "string", "TOML rejects a string one byte above its configured limit");
    toml_limits = {};
    toml_limits.max_key_bytes = 4;
    passed &= expect_error(toml::deserialize<ScalarRecord>("value = 1\n", toml_limits),
                           "key", "TOML enforces the key byte limit");
    toml_limits.max_key_bytes = 6;
    passed &= expect(toml::deserialize<ScalarRecord>("value = 1\n", toml_limits).has_value(),
                     "TOML accepts a key exactly at its configured limit");
    toml_limits = {};
    toml_limits.max_nodes = 3;
    passed &= expect(toml::deserialize<ValuesRecord>("values = [1, 2]\n", toml_limits).has_value(),
                     "TOML accepts exactly the configured node count");
    toml_limits.max_nodes = 2;
    passed &= expect_error(toml::deserialize<ValuesRecord>("values = [1, 2]\n", toml_limits),
                           "node", "TOML rejects one node above the configured limit");
    toml_limits = {};
    toml_limits.max_array_items = 2;
    passed &= expect(toml::deserialize<ValuesRecord>("values = [1, 2]\n", toml_limits).has_value(),
                     "TOML accepts exactly the configured array item limit");
    const std::string toml_limit_input = "value = 1\n";
    toml_limits = {};
    toml_limits.max_input_bytes = toml_limit_input.size();
    passed &= expect(toml::deserialize<ScalarRecord>(toml_limit_input, toml_limits).has_value(),
                     "TOML accepts an input exactly at the configured byte limit");
    toml_limits.max_input_bytes = toml_limit_input.size() - 1;
    passed &= expect_error(toml::deserialize<ScalarRecord>(toml_limit_input, toml_limits),
                           "input", "TOML rejects an input one byte above the configured limit");


    return passed ? 0 : 1;
}
