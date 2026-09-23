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

#include <serde/reflect/reflect_macros.hpp>

namespace {

enum class level : std::uint8_t { low = 1, high = 2 };

struct credentials {
    std::string user;
    std::string token;
    bool operator==(const credentials&) const = default;
};

#define CREDENTIAL_FIELDS(X) X(user) X(token)
REFLECT_FIELDS(credentials, CREDENTIAL_FIELDS)
#undef CREDENTIAL_FIELDS

struct document {
    std::string title;
    bool enabled{};
    std::int64_t signed_value{};
    std::uint64_t unsigned_value{};
    double ratio{};
    level priority{level::low};
    std::optional<std::string> note;
    std::vector<int> values;
    std::array<std::string, 2> labels{};
    std::map<std::string, int> counts;
    credentials auth;
    json::date day;
    json::time clock;
    json::local_date_time local_time;
    json::offset_date_time instant;
    bool operator==(const document&) const = default;
};

#define DOCUMENT_FIELDS(X) \
    X(title) X(enabled) X(signed_value) X(unsigned_value) X(ratio) \
    X(priority) X(note) X(values) X(labels) X(counts) X(auth) X(day) \
    X(clock) X(local_time) X(instant)
REFLECT_FIELDS(document, DOCUMENT_FIELDS)
#undef DOCUMENT_FIELDS

struct required {
    int value{};
};

#define REQUIRED_FIELDS(X) X(value)
REFLECT_FIELDS(required, REQUIRED_FIELDS)
#undef REQUIRED_FIELDS

struct OptionalValue {
    std::optional<int> value;
};

#define OPTIONAL_FIELDS(X) X(value)
REFLECT_FIELDS(OptionalValue, OPTIONAL_FIELDS)
#undef OPTIONAL_FIELDS

struct wide_record {
    int f00{}, f01{}, f02{}, f03{}, f04{}, f05{}, f06{}, f07{}, f08{}, f09{};
    int f10{}, f11{}, f12{}, f13{}, f14{}, f15{}, f16{}, f17{}, f18{}, f19{};
    std::optional<int> note = 42;
};

#define WIDE_FIELDS(X) \
    X(f00) X(f01) X(f02) X(f03) X(f04) X(f05) X(f06) X(f07) X(f08) X(f09) \
    X(f10) X(f11) X(f12) X(f13) X(f14) X(f15) X(f16) X(f17) X(f18) X(f19) X(note)
REFLECT_FIELDS(wide_record, WIDE_FIELDS)

struct static_alias_record : wide_record {};
#define STATIC_ALIAS_FIELDS(X) WIDE_FIELDS(X) X(f18, "unknown71") X(f19, "f00")
REFLECT_FIELDS(static_alias_record, STATIC_ALIAS_FIELDS)
#undef STATIC_ALIAS_FIELDS
#undef WIDE_FIELDS

struct runtime_macro_record {
    int first{}, second{}, third{}, fourth{}, fifth{};
};

std::string macro_field_name = "before";
#define RUNTIME_MACRO_FIELDS(X) \
    X(first, macro_field_name) X(second) X(third) X(fourth) X(fifth)
REFLECT_FIELDS(runtime_macro_record, RUNTIME_MACRO_FIELDS)
#undef RUNTIME_MACRO_FIELDS

struct const_record {
    const int value = 1;
};

#define CONST_FIELDS(X) X(value)
REFLECT_FIELDS(const_record, CONST_FIELDS)
#undef CONST_FIELDS

struct shared_name {
    int left{};
    int right{};
};

auto reflect_fields(const shared_name&) {
    return std::tuple{json::make_field("value", &shared_name::left),
                      json::make_field("value", &shared_name::right)};
}

struct runtime_record {
    std::array<int, 20> values{};
};

struct runtime_field {
    std::string name;
    std::size_t index;
    int& get(runtime_record& value) const { return value.values[index]; }
};

std::string runtime_prefix = "before";

auto reflect_fields(const runtime_record&) {
    std::array<runtime_field, 20> descriptors;
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        descriptors[index] = {runtime_prefix + std::to_string(index), index};
    }
    return descriptors;
}

bool expect(bool condition, std::string_view name) {
    if (!condition) std::println("test_json: {}", name);
    return condition;
}

template <class T>
bool expect_error(const json::result<T>& value, std::string_view fragment,
                  std::string_view name) {
    if (!value && value.error().find(fragment) != std::string::npos) {
        return true;
    }
    std::println("test_json: {}: {}", name,
                 value ? "unexpected success" : value.error());
    return false;
}

std::string numbered_object(std::size_t count, bool duplicate = false,
                            bool escaped = false) {
    std::string text = "{";
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) text += ',';
        text += '"';
        if (duplicate && index + 1 == count) text += escaped ? "\\u006b0" : "k0";
        else text += "k" + std::to_string(index);
        text += "\":" + std::to_string(index);
    }
    return text + '}';
}

std::string reflected_object(std::string_view prefix, std::size_t count,
                             bool padded = false, std::string_view extra = {}) {
    std::string text = "{";
    for (std::size_t remaining = count; remaining != 0; --remaining) {
        const auto index = remaining - 1;
        if (remaining != count) text += ',';
        text += '"';
        text += prefix;
        if (padded && index < 10) text += '0';
        text += std::to_string(index) + "\":" + std::to_string(index);
    }
    text += extra;
    return text + '}';
}

}  // namespace

int main() {
    const document original{
        "JSON \"fixture\"\n", true, std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::uint64_t>::max(), 0.125, level::high, std::nullopt,
        {1, 2, 3}, {"left", "right"}, {{"critical", 90}, {"warning", 70}},
        {"Ada", "secret"}, {2026, 9, 8}, {9, 30, 15, "123"},
        {{2026, 9, 8}, {9, 30, 15, "456"}},
        {{{2026, 9, 8}, {9, 30, 15, "789"}}, -480}};

    bool passed = true;
    const auto serialized = json::serialize(original);
    passed &= static_cast<bool>(serialized);
    if (serialized) {
        const auto parsed = json::deserialize<document>(*serialized);
        passed &= parsed && *parsed == original;
        if (!parsed) std::println("test_json: round-trip: {}", parsed.error());
        passed &= *serialized ==
                  "{\"auth\":{\"token\":\"secret\",\"user\":\"Ada\"},\""
                  "clock\":\"09:30:15.123\",\"counts\":{\"critical\":90,\"warning\":70},\""
                  "day\":\"2026-09-08\",\"enabled\":true,\"instant\":\"2026-09-08T09:30:15.789-08:00\",\""
                  "labels\":[\"left\",\"right\"],\"local_time\":\"2026-09-08T09:30:15.456\",\"note\":null,\""
                  "priority\":2,\"ratio\":0.125,\"signed_value\":-9223372036854775808,\""
                  "title\":\"JSON \\\"fixture\\\"\\n\",\"unsigned_value\":18446744073709551615,\""
                  "values\":[1,2,3]}";
    }

    json::SerializeOptions pretty_options;
    pretty_options.pretty = true;
    const auto pretty = json::serialize(required{7}, pretty_options);
    passed &= pretty && *pretty == "{\n  \"value\": 7\n}";

    const auto unicode = json::deserialize<std::string>(R"("\uD83D\uDE80")");
    passed &= unicode && *unicode == "\xF0\x9F\x9A\x80";
    passed &= json::deserialize<std::string>(R"("\uD800")").has_value() == false;
    passed &= json::deserialize<required>(R"({"value":1,})").has_value() == false;
    passed &= json::deserialize<required>(R"({"value":1,"value":2})").has_value() == false;
    json::ParseOptions first_wins;
    first_wins.duplicate_keys = json::DuplicateKeyPolicy::first_wins;
    const auto duplicated = json::deserialize<required>(R"({"value":1,"value":2})", first_wins);
    passed &= duplicated && duplicated->value == 1;
    json::ParseOptions depth_only;
    depth_only.max_depth = 64;
    passed &= json::deserialize<required>(R"({"value":1,"value":2})", depth_only).has_value() == false;
    passed &= json::deserialize<required>(R"({"value":01})").has_value() == false;
    passed &= json::deserialize<required>(R"({"value":1} trailing)").has_value() == false;
    passed &= static_cast<bool>(json::deserialize<required>(R"({"value":1,"extra":2})"));
    passed &= json::deserialize<required>(std::string("{\"value\":\xFF}")).has_value() == false;
    passed &= json::deserialize<std::uint64_t>("18446744073709551616").has_value() == false;
    passed &= json::deserialize<double>("1e400").has_value() == false;
    passed &= json::deserialize<json::offset_date_time>(R"("2024-01-01T00:00:00+")").has_value() == false;

    json::ParseOptions strict;
    strict.unknown_fields = json::UnknownFieldPolicy::reject;
    passed &= expect_error(json::deserialize<required>(R"({"value":1,"extra":2})", strict),
                           "unknown field", "strict unknown field policy");
    passed &= expect_error(json::deserialize<required>(R"({})"), "missing field",
                           "missing required field");
    passed &= expect_error(json::deserialize<required>(R"({"value":"one"})"),
                           "integer", "type mismatch");
    const auto null_optional = json::deserialize<OptionalValue>(R"({"value":null})");
    passed &= null_optional && !null_optional->value;

    for (const auto count : {16UZ, 17UZ, 4096UZ}) {
        const auto unique = json::parse(numbered_object(count));
        passed &= expect(unique && unique->size() == count, "unique object around duplicate-key threshold");
        for (const bool escaped : {false, true}) {
            const auto source = numbered_object(count, true, escaped);
            passed &= expect_error(json::parse(source), "duplicate object key", "duplicate object keys");
            const auto accepted = json::parse(source, first_wins);
            passed &= expect(accepted && accepted->at("k0").as_int64() &&
                                 *accepted->at("k0").as_int64() == 0,
                             "duplicate keys retain the first value");
            const auto ignored = "{\"value\":1,\"ignored\":" + source + '}';
            passed &= expect_error(json::deserialize<required>(ignored), "duplicate object key",
                                   "duplicates in ignored objects are rejected");
        }
    }

    json::ParseOptions duplicate_only;
    duplicate_only.enforce_document_limits = false;
    passed &= expect_error(json::parse(numbered_object(17, true, true), duplicate_only),
                           "duplicate object key", "duplicate policy without document limits");
    json::ParseOptions wide_limit;
    wide_limit.max_object_members = 16;
    passed &= expect_error(json::parse(numbered_object(17), wide_limit), "member count",
                           "wide object member limit");
    wide_limit = {};
    wide_limit.max_nodes = 17;
    passed &= expect_error(json::parse(numbered_object(17), wide_limit), "node count",
                           "wide object node limit");

    const auto reversed_source = reflected_object("f", 20, true);
    const auto reversed = json::deserialize<wide_record>(reversed_source, strict);
    passed &= expect(reversed && reversed->f00 == 0 && reversed->f19 == 19 &&
                         reversed->note == 42,
                     "reordered reflected fields and missing optional default");
    const auto reflected_null = json::deserialize<wide_record>(
        reflected_object("f", 20, true, ",\"note\":null"), strict);
    passed &= expect(reflected_null && !reflected_null->note, "wide reflected optional null");
    auto strict_first_wins = strict;
    strict_first_wins.duplicate_keys = json::DuplicateKeyPolicy::first_wins;
    const auto reflected_duplicate = json::deserialize<wide_record>(
        reflected_object("f", 20, true, ",\"f19\":\"ignored\""), strict_first_wins);
    passed &= expect(reflected_duplicate && reflected_duplicate->f19 == 19,
                     "reordered reflected first-wins value");
    const auto static_aliases = json::deserialize<static_alias_record>(
        reflected_object("f", 20, true, ",\"unknown71\":71,\"f00\":\"ignored\""), strict_first_wins);
    passed &= expect(static_aliases && static_aliases->f18 == 71 && static_aliases->f19 == 0,
                     "static field hash collisions and duplicate descriptor names");
    passed &= expect_error(json::deserialize<wide_record>(reflected_object("f", 19, true), strict),
                           "value is missing field 'f19'", "indexed missing required field");
    passed &= expect_error(json::deserialize<wide_record>(
                               reflected_object("f", 20, true, ",\"unknown\":1"), strict),
                           "value contains unknown field 'unknown'", "indexed unknown field");
    passed &= expect_error(json::deserialize<wide_record>(R"({"f19":"bad","f00":"bad","unknown":1})", strict),
                           "value.f00 must be a JSON integer", "descriptor-order error precedence");
    passed &= expect_error(json::deserialize<std::vector<wide_record>>(
                               '[' + reflected_object("f", 19, true) + ']'),
                           "value[0] is missing field 'f19'", "indexed nested error path");
    const auto shared = json::deserialize<shared_name>(R"({"value":7,"value":8})", strict_first_wins);
    passed &= expect(shared && shared->left == 7 && shared->right == 7,
                     "duplicate descriptor names read the same first value");
    const auto runtime_before = json::deserialize<runtime_record>(reflected_object("before", 20), strict);
    runtime_prefix = "after";
    const auto runtime_after = json::deserialize<runtime_record>(reflected_object("after", 20), strict);
    passed &= expect(runtime_before && runtime_after && runtime_before->values[19] == 19 &&
                         runtime_after->values[19] == 19,
                     "runtime reflected names are rebuilt for every decode");
    static_assert(reflect::StaticReflectable<wide_record>);
    static_assert(!reflect::StaticReflectable<runtime_macro_record>);
    const auto macro_before = json::deserialize<runtime_macro_record>(
        R"({"before":1,"second":2,"third":3,"fourth":4,"fifth":5})", strict);
    macro_field_name = "after";
    const auto macro_after = json::deserialize<runtime_macro_record>(
        R"({"after":6,"second":2,"third":3,"fourth":4,"fifth":5})", strict);
    passed &= expect(macro_before && macro_after && macro_before->first == 1 && macro_after->first == 6,
                     "macro runtime field names are not cached");
    passed &= expect_error(json::deserialize<const_record>(R"({"value":2})"),
                           "field 'value' is not assignable", "nonassignable reflected member");

    json::ParseOptions limited;
    limited.max_array_items = 2;
    passed &= expect_error(json::deserialize<std::vector<int>>("[1,2,3]", limited),
                           "item count", "array limit");
    limited = {};
    limited.max_depth = 2;
    passed &= expect_error(json::deserialize<std::vector<std::vector<int>>>("[[1]]", limited),
                           "depth", "depth limit");
    limited = {};
    limited.max_input_bytes = 2;
    passed &= expect_error(json::deserialize<required>(R"({"value":1})", limited),
                           "input", "input limit");

    passed &= expect_error(json::serialize(std::numeric_limits<double>::infinity()),
                           "non-finite", "non-finite serialization");
    json::SerializeOptions null_non_finite;
    null_non_finite.non_finite = json::NonFinitePolicy::null_value;
    const auto null_float = json::serialize(std::numeric_limits<double>::infinity(), null_non_finite);
    passed &= null_float && *null_float == "null";
    json::SerializeOptions limited_output;
    limited_output.max_output_bytes = 2;
    passed &= expect_error(json::serialize(required{123}, limited_output), "output",
                           "output limit");

    const auto leap_day = json::deserialize<json::date>(R"("2024-02-29")");
    passed &= leap_day && *leap_day == json::date{2024, 2, 29};
    passed &= json::deserialize<json::date>(R"("2024-02-30")").has_value() == false;

    const auto root = json::parse(R"({"title":"serde","values":[1,true,null]})");
    passed &= static_cast<bool>(root);
    if (root) {
        passed &= root->is_object() && root->size() == 2 && root->contains("title");
        passed &= root->at("title").as_string() && *root->at("title").as_string() == "serde";
        passed &= root->at("values").is_array() && root->at("values").size() == 3;
        passed &= root->at("values").at(0).as_int64() && *root->at("values").at(0).as_int64() == 1;
        passed &= root->at("values").at(1).as_bool() && *root->at("values").at(1).as_bool();
        passed &= root->at("values").at(2).is_null();
        passed &= !root->at("missing").valid();
    }

    return passed ? 0 : 1;
}
