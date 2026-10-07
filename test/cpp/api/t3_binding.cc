#include <galay/cpp/galay-api/binding.h>
#include <serde/reflect/reflect_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace fixture {

enum class Mode : std::uint8_t { active = 1, disabled = 2 };
constexpr auto reflect_enum(std::type_identity<Mode>) {
    return reflect::enum_descriptor<Mode, 2>{reflect::enum_encoding::string,
        {{{Mode::active, "active"}, {Mode::disabled, "disabled"}}}};
}
enum class Code : std::uint64_t { one = 1, maximum = UINT64_MAX };
constexpr auto reflect_enum(std::type_identity<Code>) {
    return reflect::enum_descriptor<Code, 2>{reflect::enum_encoding::underlying,
        {{{Code::one, "one"}, {Code::maximum, "maximum"}}}};
}
enum class Flag : bool { off = false, on = true };
constexpr auto reflect_enum(std::type_identity<Flag>) {
    return reflect::enum_descriptor<Flag, 2>{reflect::enum_encoding::underlying,
        {{{Flag::off, "off"}, {Flag::on, "on"}}}};
}

struct Scalars {
    std::uint64_t id{};
    std::int8_t small{};
    double ratio{};
    bool enabled{};
    std::string text;
    Mode mode = Mode::active;
    Code code = Code::one;
    Flag flag = Flag::off;
    std::optional<int> retries = 3;
};
#define SCALAR_FIELDS(X) \
    X(id, "external-id") X(small) X(ratio) X(enabled) \
    X(text, "text", (reflect::field_options<std::string>{.min_length = 1, .max_length = 3})) \
    X(mode) X(code) X(flag) \
    X(retries, "retries", (reflect::field_options<std::optional<int>>{.minimum = 0, .maximum = 5}))
REFLECT_FIELDS(Scalars, SCALAR_FIELDS)
#undef SCALAR_FIELDS

struct Nested { std::string name; };
#define NESTED_FIELDS(X) X(name)
REFLECT_FIELDS(Nested, NESTED_FIELDS)
#undef NESTED_FIELDS
struct Mixed {
    int id{};
    std::optional<bool> verbose = true;
    std::string title;
    Nested nested;
    std::optional<int> retries = 3;
    int hidden{};
};
#define MIXED_FIELDS(X) \
    X(id, "user-id") X(verbose) \
    X(title, "display-title", (reflect::field_options<std::string>{.min_length = 1})) \
    X(nested) X(retries, "retries", (reflect::field_options<std::optional<int>>{.minimum = 0, .maximum = 5}))
REFLECT_FIELDS(Mixed, MIXED_FIELDS)
#undef MIXED_FIELDS
struct OptionalOnly { std::optional<int> count = 3; };
#define OPTIONAL_FIELDS(X) X(count, "count", (reflect::field_options<std::optional<int>>{.minimum = 0, .maximum = 5}))
REFLECT_FIELDS(OptionalOnly, OPTIONAL_FIELDS)
#undef OPTIONAL_FIELDS
struct InvalidDefault { std::optional<int> count = 9; };
#define INVALID_DEFAULT_FIELDS(X) X(count, "count", (reflect::field_options<std::optional<int>>{.maximum = 5}))
REFLECT_FIELDS(InvalidDefault, INVALID_DEFAULT_FIELDS)
#undef INVALID_DEFAULT_FIELDS
struct Complex { std::vector<int> values; };
#define COMPLEX_FIELDS(X) X(values)
REFLECT_FIELDS(Complex, COMPLEX_FIELDS)
#undef COMPLEX_FIELDS
struct Other { int id{}; };
struct BadMetadata { int value{}; };
#define BAD_METADATA_FIELDS(X) X(value, "value", (reflect::field_options<int>{.minimum = 5, .maximum = 1}))
REFLECT_FIELDS(BadMetadata, BAD_METADATA_FIELDS)
#undef BAD_METADATA_FIELDS
struct Empty {};
REFLECT_EMPTY(Empty)
struct AliasMembers { int value{}; };
#define ALIAS_FIELDS(X) X(value, "first") X(value, "second")
REFLECT_FIELDS(AliasMembers, ALIAS_FIELDS)
#undef ALIAS_FIELDS
struct Character { char32_t value{}; };
#define CHARACTER_FIELDS(X) X(value)
REFLECT_FIELDS(Character, CHARACTER_FIELDS)
#undef CHARACTER_FIELDS
struct Unstable { int value{}; int other{}; };
inline int descriptor_change = 0;
constexpr auto reflect_fields(std::type_identity<Unstable>) {
    return std::tuple{reflect::make_field("value", &Unstable::value,
        reflect::field_options<int>{.minimum = 0})};
}
inline auto reflect_fields(const Unstable&) {
    return std::tuple{reflect::make_field(descriptor_change == 1 ? "other-name" : "value",
        descriptor_change == 2 ? &Unstable::other : &Unstable::value,
        reflect::field_options<int>{.minimum = descriptor_change == 3 ? 5 : 0})};
}

} // namespace fixture

namespace {

using namespace galay::api;
using namespace galay::http;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

template<class T>
void rejected(const ApiResult<T>& result, ApiErrorCode code, std::string_view message) {
    if (result || result.error().code != code) {
        std::cerr << "expected " << api_error_name(code) << ", actual "
                  << (result ? "success" : api_error_name(result.error().code)) << '\n';
        if (!result) std::cerr << result.error().message << '\n';
    }
    require(!result && result.error().code == code, message);
}

HttpRequest request(std::string body = {}, std::string content_type = "application/json") {
    HttpRequest result;
    result.header().method() = HttpMethod::POST;
    result.header().version() = HttpVersion::HttpVersion_1_1;
    if (!content_type.empty()) {
        require(result.header().headerPairs().addHeaderPair("Content-Type", content_type) == kNoError,
                "set content type");
    }
    result.setBodyStr(std::move(body));
    return result;
}

void test_scalars() {
    using fixture::Scalars;
    auto binding = InputBinding<Scalars>{};
    binding.path<&Scalars::id>("id").query<&Scalars::small>("small")
        .query<&Scalars::ratio>("ratio").query<&Scalars::enabled>("enabled")
        .query<&Scalars::text>("text").query<&Scalars::mode>("mode")
        .query<&Scalars::code>("code").query<&Scalars::flag>("flag")
        .query<&Scalars::retries>("retries");
    auto plan = binding.prepare(HttpMethod::GET, "/items/:id");
    require(plan.has_value(), "prepare all scalar inputs");
    require(!plan->body_schema && plan->parameters.size() == 9, "no scalar body");
    require(plan->parameters[0].field_name == "external-id", "member pointer finds renamed field");
    require(plan->parameters[0].required && !plan->parameters[8].required, "parameter required rules");
    require(plan->parameters[7].schema.type == "boolean", "bool enum schema");

    auto req = request({}, {});
    req.setRouteParams(std::map<std::string, std::string>{{"id", "18446744073709551615"}});
    req.header().args() = {{"small", "-128"}, {"ratio", "1.25"}, {"enabled", "true"},
        {"text", "\xE4\xBD\xA0\xE5\xA5\xBD"}, {"mode", "disabled"},
        {"code", "18446744073709551615"}, {"flag", "false"}};
    const auto decoded = plan->decode(req);
    require(decoded && decoded->id == UINT64_MAX && decoded->small == -128 && decoded->ratio == 1.25,
            "full uint64 and signed boundaries");
    require(decoded->mode == fixture::Mode::disabled && decoded->code == fixture::Code::maximum &&
            decoded->flag == fixture::Flag::off && decoded->retries == 3, "enum codec and optional default");

    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
            {"small", "128"}, {"small", "-129"}, {"small", "3tail"}, {"small", " 3"},
            {"ratio", "1.2tail"}, {"ratio", "1e999"}, {"ratio", "nan"}, {"ratio", "inf"},
            {"enabled", "TRUE"}, {"enabled", "1"}, {"enabled", ""},
            {"text", ""}, {"text", "four"}, {"text", "\xFF"}, {"text", "\xED\xA0\x80"},
            {"mode", "1"}, {"mode", "unknown"}, {"code", "2"}, {"code", "-1"},
            {"flag", "0"}, {"retries", "6"}, {"retries", "null"}, {"retries", ""}}) {
        auto invalid = req.clone();
        invalid.header().args()[key] = value;
        rejected(plan->decode(invalid), ApiErrorCode::bad_request, "bad scalar must fail");
    }
    for (const auto value : {"18446744073709551616", "-1", "1x", "", "+1"}) {
        auto invalid = req.clone();
        invalid.setRouteParams(std::map<std::string, std::string>{{"id", value}});
        rejected(plan->decode(invalid), ApiErrorCode::bad_request, "invalid path integer");
    }
    auto missing = req.clone();
    missing.setRouteParams(std::map<std::string, std::string>{});
    rejected(plan->decode(missing), ApiErrorCode::bad_request, "missing path parameter");
    missing = req.clone();
    require(missing.header().args().erase("small") == 1, "erase mandatory query");
    rejected(plan->decode(missing), ApiErrorCode::bad_request, "missing mandatory query");
    auto unexpected_body = req.clone();
    unexpected_body.setBodyStr(std::string("{}"));
    rejected(plan->decode(unexpected_body), ApiErrorCode::bad_request, "body rejected when no body schema");
}

void test_mixed_body() {
    using fixture::Mixed;
    auto binding = InputBinding<Mixed>{};
    binding.path<&Mixed::id>("id").query<&Mixed::verbose>("verbose");
    auto plan = binding.prepare(HttpMethod::POST, "/items/:id");
    require(plan && plan->body_schema && plan->body_required, "mixed required body");
    require(!plan->body_schema->properties.contains("user-id") &&
            !plan->body_schema->properties.contains("verbose") &&
            plan->body_schema->properties.contains("display-title"), "only remaining fields in schema");
    auto req = request(R"({"display-title":"Ada","nested":{"name":"inner"}})",
                       "Application/JSON; charset=utf-8");
    req.setRouteParams(std::map<std::string, std::string>{{"id", "7"}});
    auto decoded = plan->decode(req);
    require(decoded && decoded->id == 7 && decoded->verbose == true && decoded->title == "Ada" &&
            decoded->nested.name == "inner" && decoded->retries == 3, "mixed body does not repeat query/path");
    req.setBodyStr(std::string(R"({"display-title":"Ada","nested":{"name":"inner"},"retries":null})"));
    decoded = plan->decode(req);
    require(decoded && !decoded->retries, "explicit optional null clears default");

    for (const auto body : {"", "{", "null", "[]", "{}",
            R"({"display-title":null,"nested":{"name":"inner"}})",
            R"({"display-title":"","nested":{"name":"inner"}})",
            R"({"display-title":"Ada","nested":{"name":"inner"},"user-id":7})",
            R"({"display-title":"Ada","nested":{"name":"inner"},"verbose":true})",
            R"({"display-title":"Ada","nested":{"name":"inner","unknown":1}})",
            R"({"display-title":"Ada","nested":{"name":"inner"},"unknown":1})",
            R"({"display-title":"Ada","display-title":"Bob","nested":{"name":"inner"}})",
            R"({"display-title":"Ada","nested":{"name":"inner"},"retries":6})"}) {
        auto invalid = req.clone();
        invalid.setBodyStr(std::string(body));
        rejected(plan->decode(invalid), ApiErrorCode::bad_request, "invalid/unknown body rejected");
    }
    for (const auto type : {"", "text/plain", "application/jsonp", "application/problem+json"}) {
        auto invalid = req.clone();
        require(invalid.header().headerPairs().removeHeaderPair("Content-Type") == kNoError, "remove type");
        if (!std::string_view(type).empty()) {
            require(invalid.header().headerPairs().addHeaderPair("Content-Type", type) == kNoError, "set type");
        }
        rejected(plan->decode(invalid), ApiErrorCode::unsupported_media_type, "JSON content type required");
    }
    auto repeated_type = req.clone();
    require(repeated_type.header().headerPairs().addHeaderPair("Content-Type", "text/plain") == kNoError,
            "append repeated content type");
    rejected(plan->decode(repeated_type), ApiErrorCode::unsupported_media_type,
             "repeated content type is rejected rather than partially accepted");
}

void test_optional_defaults() {
    auto query = InputBinding<fixture::OptionalOnly>{};
    query.query<&fixture::OptionalOnly::count>("count");
    auto query_plan = query.prepare(HttpMethod::GET, "/optional");
    require(query_plan.has_value(), "prepare optional query");
    auto absent = request({}, {});
    auto decoded = query_plan->decode(absent);
    require(decoded && decoded->count == 3, "missing query retains default");
    auto body_plan = InputBinding<fixture::OptionalOnly>{}.prepare(HttpMethod::POST, "/optional");
    require(body_plan && !body_plan->body_required, "all optional body is not required");
    decoded = body_plan->decode(absent);
    require(decoded && decoded->count == 3, "missing optional body retains default");

    auto bad_query = InputBinding<fixture::InvalidDefault>{};
    bad_query.query<&fixture::InvalidDefault::count>("count");
    auto bad_query_plan = bad_query.prepare(HttpMethod::GET, "/invalid-default");
    require(bad_query_plan.has_value(), "default value is validated at decode, not registration");
    rejected(bad_query_plan->decode(absent), ApiErrorCode::bad_request, "invalid missing-query default rejected");
    auto bad_body_plan = InputBinding<fixture::InvalidDefault>{}.prepare(HttpMethod::POST, "/invalid-default");
    require(bad_body_plan.has_value(), "prepare optional body with invalid default");
    rejected(bad_body_plan->decode(absent), ApiErrorCode::bad_request, "invalid missing-body default rejected");
}

void test_prepare_errors() {
    using fixture::Mixed;
    auto duplicate = InputBinding<Mixed>{};
    duplicate.path<&Mixed::id>("id").query<&Mixed::id>("id-query");
    rejected(duplicate.prepare(HttpMethod::POST, "/:id"), ApiErrorCode::invalid_binding, "duplicate member source");
    auto duplicate_query = InputBinding<Mixed>{};
    duplicate_query.query<&Mixed::id>("q").query<&Mixed::verbose>("q");
    rejected(duplicate_query.prepare(HttpMethod::POST, "/"), ApiErrorCode::invalid_binding, "duplicate query key");
    auto missing = InputBinding<Mixed>{};
    rejected(missing.prepare(HttpMethod::POST, "/:id"), ApiErrorCode::invalid_binding, "unmapped path");
    missing.path<&Mixed::id>("other");
    rejected(missing.prepare(HttpMethod::POST, "/:id"), ApiErrorCode::invalid_binding, "wrong path mapping");
    auto hidden = InputBinding<Mixed>{};
    hidden.query<&Mixed::hidden>("hidden");
    rejected(hidden.prepare(HttpMethod::POST, "/"), ApiErrorCode::invalid_binding, "unregistered member");
    auto wrong_owner = InputBinding<Mixed>{};
    wrong_owner.query<&fixture::Other::id>("id");
    rejected(wrong_owner.prepare(HttpMethod::POST, "/"), ApiErrorCode::invalid_binding, "member from wrong DTO");
    auto complex = InputBinding<fixture::Complex>{};
    complex.query<&fixture::Complex::values>("values");
    rejected(complex.prepare(HttpMethod::GET, "/"), ApiErrorCode::invalid_binding, "query container unsupported");
    auto optional_path = InputBinding<fixture::OptionalOnly>{};
    optional_path.path<&fixture::OptionalOnly::count>("count");
    rejected(optional_path.prepare(HttpMethod::GET, "/:count"), ApiErrorCode::invalid_binding, "optional path unsupported");
    for (const auto name : {"", "bad\nname", "\xFF"}) {
        auto bad = InputBinding<Mixed>{};
        bad.query<&Mixed::id>(name);
        rejected(bad.prepare(HttpMethod::POST, "/"), ApiErrorCode::invalid_binding, "invalid source name");
    }
    for (const auto method : {HttpMethod::GET, HttpMethod::HEAD}) {
        auto body = InputBinding<fixture::OptionalOnly>{}.prepare(method, "/");
        rejected(body, ApiErrorCode::invalid_binding, "GET/HEAD cannot have remaining body fields");
    }
    auto bad_metadata = InputBinding<fixture::BadMetadata>{}.prepare(HttpMethod::POST, "/");
    require(!bad_metadata, "invalid field metadata rejected in prepare");
    rejected(InputBinding<fixture::AliasMembers>{}.prepare(HttpMethod::POST, "/"),
             ApiErrorCode::invalid_binding, "same member reflected under distinct body names");
    auto scalar_input = InputBinding<int>{}.prepare(HttpMethod::POST, "/");
    require(!scalar_input, "input must be a static DTO or NoInput");
    auto no_input = InputBinding<NoInput>{}.prepare(HttpMethod::GET, "/health");
    auto empty = InputBinding<fixture::Empty>{}.prepare(HttpMethod::GET, "/empty");
    require(no_input && empty && !no_input->body_schema && !empty->body_schema, "empty inputs have no body");
    auto unexpected = request("{}", {});
    rejected(no_input->decode(unexpected), ApiErrorCode::bad_request, "NoInput rejects unexpected body");
    rejected(empty->decode(unexpected), ApiErrorCode::bad_request, "empty DTO rejects unexpected body");
}

void test_descriptor_stability() {
    static_assert(reflect::StaticReflectable<fixture::Unstable>);
    fixture::descriptor_change = 0;
    const auto stable = InputBinding<fixture::Unstable>{}.prepare(HttpMethod::POST, "/stable");
    require(stable.has_value(), "matching public static/runtime field descriptors accepted");
    auto req = request(R"({"value":7})");
    require(stable->decode(req).has_value(), "stable descriptor body decoded");
    for (int change = 1; change <= 3; ++change) {
        fixture::descriptor_change = change;
        rejected(InputBinding<fixture::Unstable>{}.prepare(HttpMethod::POST, "/stable"),
                 ApiErrorCode::invalid_binding, "runtime name/pointer/options divergence rejected at prepare");
        rejected(stable->decode(req), ApiErrorCode::invalid_binding,
                 "contract changing after prepare rejected before binding");
    }
    fixture::descriptor_change = 0;
}

void test_character_integer_codec() {
    auto binding = InputBinding<fixture::Character>{};
    binding.query<&fixture::Character::value>("value");
    const auto plan = binding.prepare(HttpMethod::GET, "/character");
    require(plan.has_value(), "character integer uses serde numeric codec");
    auto req = request({}, {});
    req.header().args() = {{"value", "4294967295"}};
    const auto decoded = plan->decode(req);
    require(decoded && decoded->value == std::numeric_limits<char32_t>::max(), "character integer full range");
    req.header().args()["value"] = "4294967296";
    rejected(plan->decode(req), ApiErrorCode::bad_request, "character integer overflow rejected");
}

} // namespace

int main() {
    test_scalars();
    test_mixed_body();
    test_optional_defaults();
    test_prepare_errors();
    test_descriptor_stability();
    test_character_integer_codec();
    std::cout << "P2 binding behavior PASS\n";
}
