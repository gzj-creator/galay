#include <galay/cpp/galay-api/schema.h>
#include <serde/json/json.hpp>
#include <serde/reflect/reflect_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fixture {

enum class Mode { Active, Disabled };
constexpr auto reflect_enum(std::type_identity<Mode>) {
    return reflect::enum_descriptor<Mode, 2>{
        reflect::enum_encoding::string,
        {{{Mode::Active, "active"}, {Mode::Disabled, "disabled"}}}};
}

enum class Code : std::uint64_t { Large = 9007199254740993ULL, Maximum = UINT64_MAX };
constexpr auto reflect_enum(std::type_identity<Code>) {
    return reflect::enum_descriptor<Code, 2>{
        reflect::enum_encoding::underlying,
        {{{Code::Large, "large"}, {Code::Maximum, "maximum"}}}};
}

enum class Flag : bool { Off = false, On = true };
constexpr auto reflect_enum(std::type_identity<Flag>) {
    return reflect::enum_descriptor<Flag, 2>{
        reflect::enum_encoding::underlying,
        {{{Flag::Off, "off"}, {Flag::On, "on"}}}};
}

enum class PlainFlag : bool { Off = false, On = true };
enum class Empty { Value };
constexpr auto reflect_enum(std::type_identity<Empty>) {
    return reflect::enum_descriptor<Empty, 0>{reflect::enum_encoding::string, {}};
}
enum class DuplicateValue { One, Two };
constexpr auto reflect_enum(std::type_identity<DuplicateValue>) {
    return reflect::enum_descriptor<DuplicateValue, 2>{
        reflect::enum_encoding::string,
        {{{DuplicateValue::One, "one"}, {DuplicateValue::One, "two"}}}};
}
enum class DuplicateName { One, Two };
constexpr auto reflect_enum(std::type_identity<DuplicateName>) {
    return reflect::enum_descriptor<DuplicateName, 2>{
        reflect::enum_encoding::underlying,
        {{{DuplicateName::One, "same"}, {DuplicateName::Two, "same"}}}};
}
enum class BadName { Value };
constexpr auto reflect_enum(std::type_identity<BadName>) {
    return reflect::enum_descriptor<BadName, 1>{
        reflect::enum_encoding::string, {{{BadName::Value, "\xED\xA0\x80"}}}};
}
enum class BadEncoding { Value };
constexpr auto reflect_enum(std::type_identity<BadEncoding>) {
    return reflect::enum_descriptor<BadEncoding, 1>{
        static_cast<reflect::enum_encoding>(99), {{{BadEncoding::Value, "value"}}}};
}

enum class RuntimeMode { Value };
auto reflect_enum(std::type_identity<RuntimeMode>) {
    return reflect::enum_descriptor<RuntimeMode, 1>{
        reflect::enum_encoding::string, {{{RuntimeMode::Value, "value"}}}};
}
enum class MutableName { Value };
inline char mutable_name[] = "value";
constexpr auto reflect_enum(std::type_identity<MutableName>) {
    return reflect::enum_descriptor<MutableName, 1>{
        reflect::enum_encoding::string, {{{MutableName::Value, {mutable_name, 5}}}}};
}
enum class ChangedContract { Value };
constexpr auto reflect_enum(std::type_identity<ChangedContract>) {
    if consteval {
        return reflect::enum_descriptor<ChangedContract, 1>{
            reflect::enum_encoding::string, {{{ChangedContract::Value, "static"}}}};
    } else {
        return reflect::enum_descriptor<ChangedContract, 1>{
            reflect::enum_encoding::string, {{{ChangedContract::Value, "runtime"}}}};
    }
}

struct Leaf { std::string text; };
#define LEAF_FIELDS(X) X(text, "text", (reflect::field_options<std::string>{.min_length = 1, .max_length = 5}))
REFLECT_FIELDS(Leaf, LEAF_FIELDS)
#undef LEAF_FIELDS

struct Document {
    std::string name;
    std::uint64_t id{};
    std::optional<int> age = 25;
    std::vector<Leaf> entries;
    std::array<int, 2> fixed{};
    std::map<std::string, std::optional<Leaf>> attributes;
    std::optional<Mode> mode;
};
#define DOCUMENT_FIELDS(X) \
    X(name, "display\"name", (reflect::field_options<std::string>{.description = "Name\n\"quoted\"", .min_length = 1, .max_length = 40})) \
    X(id, "external-id", (reflect::field_options<std::uint64_t>{.minimum = 9007199254740993ULL, .maximum = UINT64_MAX})) \
    X(age, "age", (reflect::field_options<std::optional<int>>{.minimum = 18, .maximum = 120})) \
    X(entries, "entries", (reflect::field_options<std::vector<Leaf>>{.min_items = 1, .max_items = 3})) \
    X(fixed, "fixed", (reflect::field_options<std::array<int, 2>>{.min_items = 1, .max_items = 3})) \
    X(attributes, "attributes", (reflect::field_options<std::map<std::string, std::optional<Leaf>>>{.min_items = 1, .max_items = 4})) \
    X(mode)
REFLECT_FIELDS(Document, DOCUMENT_FIELDS)
#undef DOCUMENT_FIELDS

struct NonDefault {
    NonDefault() = delete;
    int value;
};
#define NONDEFAULT_FIELDS(X) X(value)
REFLECT_FIELDS(NonDefault, NONDEFAULT_FIELDS)
#undef NONDEFAULT_FIELDS

struct Recursive { std::vector<Recursive> children; };
#define RECURSIVE_FIELDS(X) X(children)
REFLECT_FIELDS(Recursive, RECURSIVE_FIELDS)
#undef RECURSIVE_FIELDS

struct Dynamic { int value{}; };
inline std::string dynamic_name = "runtime";
#define DYNAMIC_FIELDS(X) X(value, dynamic_name)
REFLECT_FIELDS(Dynamic, DYNAMIC_FIELDS)
#undef DYNAMIC_FIELDS

struct DuplicateFields { int first{}; int second{}; };
#define DUPLICATE_FIELDS(X) X(first, "same") X(second, "same")
REFLECT_FIELDS(DuplicateFields, DUPLICATE_FIELDS)
#undef DUPLICATE_FIELDS

struct BadFieldName { int value{}; };
#define BAD_FIELDS(X) X(value, "\xFF")
REFLECT_FIELDS(BadFieldName, BAD_FIELDS)
#undef BAD_FIELDS

struct EmptyDto {};
REFLECT_EMPTY(EmptyDto)

template<class T>
struct Box { T value; };

} // namespace fixture

namespace {

using namespace galay::api;

bool expect(bool condition, std::string_view name) {
    if (!condition) std::cerr << "FAIL: " << name << '\n';
    return condition;
}

template<class T>
bool error_is(const ApiResult<T>& result, ApiErrorCode code, std::string_view name) {
    return expect(!result && result.error().code == code && !result.error().message.empty(), name);
}

template<class T>
ApiResult<Schema> field_schema(reflect::field_options<T> options) {
    return schema_for_field(reflect::make_field("value", &fixture::Box<T>::value, options), SchemaUse::input);
}

bool has_required(const Schema& schema, std::string_view name) {
    return std::find(schema.required.begin(), schema.required.end(), name) != schema.required.end();
}

bool export_schema_file(std::string_view path, const Schema& schema) {
    const auto encoded = schema_json(schema);
    if (!encoded) {
        std::cerr << "schema export failed: " << encoded.error().message << '\n';
        return false;
    }
    std::string filename(path);
    std::FILE* file = std::fopen(filename.c_str(), "wb");
    if (file == nullptr) {
        std::cerr << "schema export could not open output file: " << filename << '\n';
        return false;
    }
    const std::size_t written = std::fwrite(encoded->data(), 1, encoded->size(), file);
    bool success = written == encoded->size();
    if (!success) {
        std::cerr << "schema export wrote a partial JSON document: " << filename << '\n';
    }
    const int closed = std::fclose(file);
    if (closed != 0) {
        std::cerr << "schema export could not close output file: " << filename << '\n';
        success = false;
    }
    return success;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 1 && argc != 3) {
        std::cerr << "usage: api_t1_schema [--export-schemas <path>]\n";
        return 2;
    }
    if (argc == 3 && std::string_view(argv[1]) != "--export-schemas") {
        std::cerr << "usage: api_t1_schema [--export-schemas <path>]\n";
        return 2;
    }
    const std::string_view export_path = argc == 3 ? std::string_view(argv[2]) : std::string_view{};
    if (argc == 3 && export_path.empty()) {
        std::cerr << "schema export path must not be empty\n";
        return 2;
    }

    bool passed = true;
    const auto boolean = schema_for<bool>(SchemaUse::input);
    const auto integer = schema_for<std::int32_t>(SchemaUse::input);
    const auto unsigned_integer = schema_for<std::uint64_t>(SchemaUse::output);
    const auto floating = schema_for<double>(SchemaUse::input);
    const auto text = schema_for<std::string>(SchemaUse::input);
    passed &= expect(boolean && boolean->type == "boolean" && !boolean->minimum, "boolean is not numeric");
    passed &= expect(integer && integer->type == "integer" && integer->format == "int32" &&
                     std::get<std::int64_t>(*integer->minimum) == INT32_MIN &&
                     std::get<std::int64_t>(*integer->maximum) == INT32_MAX, "signed range");
    passed &= expect(unsigned_integer && unsigned_integer->type == "integer" &&
                     unsigned_integer->format != "int64" &&
                     std::get<std::uint64_t>(*unsigned_integer->maximum) == UINT64_MAX, "uint64 exact range");
    passed &= expect(floating && floating->type == "number" && floating->format == "double", "number mapping");
    passed &= expect(text && text->type == "string", "string mapping");

    const auto input = schema_for<fixture::Document>(SchemaUse::input);
    const auto output = schema_for<fixture::Document>(SchemaUse::output);
    passed &= expect(input && output, "static nested DTO generation");
    if (!export_path.empty()) {
        passed &= expect(input.has_value(), "schema export input generation");
        if (input && !export_schema_file(export_path, *input)) return 1;
        if (output && !export_schema_file(std::string(export_path) + ".output.json", *output)) return 1;
        const auto numeric_enum = schema_for<fixture::Code>(SchemaUse::input);
        const auto boolean_enum = schema_for<std::optional<fixture::Flag>>(SchemaUse::output);
        passed &= expect(numeric_enum && boolean_enum, "enum schema fixtures generated");
        if (numeric_enum && !export_schema_file(std::string(export_path) + ".uint64-enum.json", *numeric_enum)) return 1;
        if (boolean_enum && !export_schema_file(std::string(export_path) + ".bool-enum.json", *boolean_enum)) return 1;
    }
    if (input && output) {
        passed &= expect(input->properties.size() == 7 && input->properties.contains("display\"name"), "serde wire names");
        passed &= expect(has_required(*input, "external-id") && !has_required(*input, "age") &&
                         !has_required(*input, "mode") && output->required.size() == 7, "input/output required differ");
        passed &= expect(input->properties.at("age").nullable && output->properties.at("age").nullable,
                         "optional is nullable in both uses");
        const auto& name = input->properties.at("display\"name");
        passed &= expect(name.description == "Name\n\"quoted\"" && name.min_length == 1 && name.max_length == 40,
                         "description and Unicode scalar length constraints");
        const auto& entries = input->properties.at("entries");
        passed &= expect(entries.type == "array" && entries.min_items == 1 && entries.max_items == 3 &&
                         entries.items && entries.items->properties.contains("text"), "vector and nested items");
        const auto& fixed = input->properties.at("fixed");
        passed &= expect(fixed.min_items == 2 && fixed.max_items == 2, "fixed array intersects field constraints");
        const auto& attributes = input->properties.at("attributes");
        passed &= expect(attributes.type == "object" && attributes.min_properties == 1 && attributes.max_properties == 4 &&
                         !attributes.min_items && attributes.additional_properties && attributes.additional_properties->nullable,
                         "map uses property counts and typed additionalProperties");
        const auto encoded = schema_json(*input);
        passed &= expect(encoded.has_value(), "schema JSON output");
        if (encoded) {
            const auto parsed = json::parse(*encoded);
            passed &= expect(parsed.has_value(), "schema JSON is valid");
            if (parsed) {
                const auto id_min = (*parsed)["properties"]["external-id"]["minimum"].as_uint64();
                const auto id_max = (*parsed)["properties"]["external-id"]["maximum"].as_uint64();
                passed &= expect(id_min && *id_min == 9007199254740993ULL && id_max && *id_max == UINT64_MAX,
                                 "uint64 values do not round through double");
                const auto mode_type = (*parsed)["properties"]["mode"]["type"];
                const auto mode_enum = (*parsed)["properties"]["mode"]["enum"];
                passed &= expect(mode_type.is_array() && mode_type.size() == 2 && mode_enum.size() == 3 &&
                                 mode_enum.at(2).is_null(), "nullable enum admits null under JSON Schema 2020-12");
                passed &= expect(!(*parsed)["properties"]["mode"].contains("nullable"), "no OpenAPI 3.0 nullable keyword");
            }
        }
    }

    const auto mode = schema_for<fixture::Mode>(SchemaUse::input);
    const auto code = schema_for<fixture::Code>(SchemaUse::input);
    const auto flag = schema_for<fixture::Flag>(SchemaUse::input);
    const auto plain_flag = schema_for<fixture::PlainFlag>(SchemaUse::input);
    passed &= expect(mode && mode->type == "string" && mode->enum_values.size() == 2 &&
                     std::get<std::string>(mode->enum_values.front()) == "active", "string enum names");
    passed &= expect(code && code->type == "integer" && code->enum_values.size() == 2 &&
                     std::get<std::uint64_t>(code->enum_values.back()) == UINT64_MAX, "numeric enum exact values");
    passed &= expect(flag && flag->type == "boolean" && !flag->minimum && flag->enum_values.size() == 2 &&
                     std::get<bool>(flag->enum_values.back()), "registered bool enum uses boolean");
    passed &= expect(plain_flag && plain_flag->type == "boolean" && plain_flag->enum_values.empty(),
                     "unregistered bool enum follows actual codec");
    const auto filtered = field_schema<fixture::Code>({.minimum = UINT64_MAX});
    passed &= expect(filtered && filtered->enum_values.size() == 1 &&
                     std::get<std::uint64_t>(filtered->enum_values.front()) == UINT64_MAX,
                     "enum and numeric bounds intersect");
    passed &= error_is(field_schema<fixture::Code>({.maximum = 1}), ApiErrorCode::kInvalidMetadata,
                       "empty enum/constraint intersection fails");

    passed &= expect(field_schema<int>({.minimum = 18, .maximum = 120}).has_value(),
                     "metadata validation does not validate a fabricated default value");
    passed &= error_is(field_schema<int>({.minimum = 4, .maximum = 3}), ApiErrorCode::kInvalidMetadata, "reversed numeric bounds");
    passed &= error_is(field_schema<int>({.min_length = 1}), ApiErrorCode::kInvalidMetadata, "inapplicable length option");
    passed &= error_is(field_schema<bool>({.minimum = 0}), ApiErrorCode::kInvalidMetadata, "bool rejects numeric bounds");
    passed &= error_is(field_schema<fixture::Flag>({.minimum = false}), ApiErrorCode::kInvalidMetadata, "bool enum rejects numeric bounds");
    passed &= error_is(field_schema<fixture::Mode>({.minimum = 0}), ApiErrorCode::kInvalidMetadata, "string enum rejects numeric bounds");
    passed &= error_is(field_schema<double>({.minimum = std::numeric_limits<double>::infinity()}),
                       ApiErrorCode::kInvalidMetadata, "infinite numeric metadata");
    passed &= error_is(field_schema<std::optional<double>>({.maximum = std::numeric_limits<double>::quiet_NaN()}),
                       ApiErrorCode::kInvalidMetadata, "optional still validates metadata");
    passed &= error_is(field_schema<std::string>({.min_length = 4, .max_length = 3}),
                       ApiErrorCode::kInvalidMetadata, "reversed string length");
    passed &= error_is(field_schema<std::vector<int>>({.min_items = 4, .max_items = 3}),
                       ApiErrorCode::kInvalidMetadata, "reversed collection count");
    passed &= error_is(field_schema<std::array<int, 2>>({.min_items = 3}),
                       ApiErrorCode::kInvalidMetadata, "fixed array incompatible minimum");
    passed &= error_is(field_schema<std::optional<std::array<int, 2>>>({.max_items = 1}),
                       ApiErrorCode::kInvalidMetadata, "optional fixed array incompatible maximum");
    passed &= error_is(field_schema<int>({.description = "\xFF"}), ApiErrorCode::kInvalidMetadata, "invalid UTF-8 description");
    const auto null_member = reflect::make_field("value", static_cast<int fixture::Box<int>::*>(nullptr));
    passed &= error_is(schema_for_field(null_member, SchemaUse::input), ApiErrorCode::kInvalidMetadata, "null field member pointer");

    passed &= error_is(schema_for<fixture::Empty>(SchemaUse::input), ApiErrorCode::kInvalidMetadata, "empty enum descriptor");
    passed &= error_is(schema_for<fixture::DuplicateValue>(SchemaUse::input), ApiErrorCode::kInvalidMetadata, "duplicate enum value");
    passed &= error_is(schema_for<fixture::DuplicateName>(SchemaUse::input), ApiErrorCode::kInvalidMetadata, "duplicate enum name");
    passed &= error_is(schema_for<fixture::BadName>(SchemaUse::input), ApiErrorCode::kInvalidMetadata, "invalid enum UTF-8");
    passed &= error_is(schema_for<fixture::BadEncoding>(SchemaUse::input), ApiErrorCode::kInvalidMetadata, "invalid enum encoding");
    passed &= error_is(schema_for<fixture::RuntimeMode>(SchemaUse::input), ApiErrorCode::kUnsupportedType, "nonstatic enum rejected");
    passed &= error_is(schema_for<fixture::MutableName>(SchemaUse::input), ApiErrorCode::kUnsupportedType, "enum borrowed mutable name rejected");
    passed &= error_is(schema_for<fixture::ChangedContract>(SchemaUse::input), ApiErrorCode::kInvalidMetadata, "runtime enum snapshot must match static contract");
    passed &= error_is(schema_for<fixture::DuplicateFields>(SchemaUse::input), ApiErrorCode::kInvalidMetadata, "duplicate DTO wire name");
    passed &= error_is(schema_for<fixture::BadFieldName>(SchemaUse::input), ApiErrorCode::kInvalidMetadata, "invalid DTO UTF-8 name");
    passed &= error_is(schema_for<fixture::Dynamic>(SchemaUse::input), ApiErrorCode::kUnsupportedType, "dynamic DTO rejected");
    passed &= error_is(schema_for<int*>(SchemaUse::input), ApiErrorCode::kUnsupportedType, "pointer rejected");
    passed &= error_is(schema_for<std::variant<int, std::string>>(SchemaUse::input), ApiErrorCode::kUnsupportedType, "variant rejected");
    passed &= error_is(schema_for<std::string_view>(SchemaUse::input), ApiErrorCode::kUnsupportedType, "non-owning input string rejected");
    passed &= expect(schema_for<std::string_view>(SchemaUse::output).has_value(), "output string_view matches serde encoder");
    passed &= error_is(schema_for<std::map<int, int>>(SchemaUse::input), ApiErrorCode::kUnsupportedType, "non-string map key rejected");
    passed &= error_is(schema_for<fixture::Recursive>(SchemaUse::input), ApiErrorCode::kInvalidSchema, "recursive inline DTO fails explicitly");
    passed &= error_is(schema_for<int>(static_cast<SchemaUse>(99)), ApiErrorCode::kInvalidMetadata, "invalid SchemaUse rejected");
    passed &= expect(schema_for<fixture::NonDefault>(SchemaUse::output).has_value(), "DTO need not be default constructible");
    const auto empty = schema_for<fixture::EmptyDto>(SchemaUse::output);
    passed &= expect(empty && empty->type == "object" && empty->properties.empty(), "empty static DTO");
    const auto unordered = schema_for<std::unordered_map<std::string, bool>>(SchemaUse::input);
    passed &= expect(unordered && unordered->additional_properties && unordered->additional_properties->type == "boolean", "unordered map");
    const auto bool_vector = schema_for<std::vector<bool>>(SchemaUse::input);
    passed &= expect(bool_vector && bool_vector->items && bool_vector->items->type == "boolean", "vector bool value type");
    const auto empty_array = schema_for<std::array<int, 0>>(SchemaUse::input);
    passed &= expect(empty_array && empty_array->min_items == 0 && empty_array->max_items == 0,
                     "zero length array preserves zero bounds");
    const auto nullable_flag = schema_for<std::optional<fixture::Flag>>(SchemaUse::output);
    passed &= expect(nullable_flag.has_value(), "optional bool enum generation");
    if (nullable_flag) {
        const auto encoded = schema_json(*nullable_flag);
        passed &= expect(encoded.has_value(), "optional bool enum output");
        if (encoded) {
            const auto parsed = json::parse(*encoded);
            passed &= expect(parsed && (*parsed)["enum"].size() == 3 && (*parsed)["enum"].at(0).is_bool() &&
                             (*parsed)["enum"].at(2).is_null(), "boolean enum values and null retain their JSON types");
        }
    }

    Schema invalid;
    invalid.type = "array";
    passed &= error_is(schema_json(invalid), ApiErrorCode::kInvalidSchema, "array without items is not a generated schema");
    invalid.type = "string";
    invalid.minimum = std::int64_t{1};
    passed &= error_is(schema_json(invalid), ApiErrorCode::kInvalidSchema, "public schema applicability checked");
    auto cycle = std::make_shared<Schema>();
    cycle->type = "array";
    cycle->items = cycle;
    passed &= error_is(schema_json(*cycle), ApiErrorCode::kInvalidSchema, "public schema pointer cycle detected");
    cycle->items.reset();
    if (passed) std::cout << "api schema contract passed\n";
    return passed ? 0 : 1;
}
