#include <serde/json/json.hpp>
#include <serde/reflect/reflect_macros.hpp>
#include <serde/toml/toml.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace consumer {

enum class Mode : std::uint8_t { Active = 1, Disabled = 2 };

constexpr auto reflect_enum(std::type_identity<Mode>) {
    return reflect::enum_descriptor<Mode, 2>{
        reflect::enum_encoding::string,
        {{{Mode::Active, "active"}, {Mode::Disabled, "disabled"}}}
    };
}

struct Policy {
    std::string name;
    std::uint64_t quota{};
    std::vector<int> ports;
    std::optional<int> retries = 3;
    Mode mode = Mode::Active;
    bool operator==(const Policy&) const = default;
};

constexpr reflect::field_options<std::string> name_options{
    .description = "Display name", .min_length = 1, .max_length = 5};
constexpr reflect::field_options<std::uint64_t> quota_options{
    .minimum = 1, .maximum = 100};
constexpr reflect::field_options<std::vector<int>> ports_options{
    .min_items = 1, .max_items = 3};
constexpr reflect::field_options<std::optional<int>> retries_options{
    .minimum = 0, .maximum = 5};

#define POLICY_FIELDS(X) \
    X(name, "display-name", (name_options)) \
    X(quota, "quota", (quota_options)) \
    X(ports, "ports", (ports_options)) \
    X(retries, "retries", (retries_options)) \
    X(mode)
REFLECT_FIELDS(Policy, POLICY_FIELDS)
#undef POLICY_FIELDS

} // namespace consumer

int main() {
    using consumer::Mode;
    using consumer::Policy;
    static_assert(reflect::StaticReflectable<Policy>);
    static_assert(reflect::EnumReflectable<Mode>);
    constexpr auto fields = reflect::static_fields<Policy>();
    static_assert(std::get<0>(fields).options.description == "Display name");

    const Policy valid{"Ada", 10, {80, 443}, 3, Mode::Active};
    const auto encoded_json = json::serialize(valid);
    assert(encoded_json);
    const auto encoded_toml = toml::serialize(valid);
    assert(encoded_toml);
    const auto json_back = json::deserialize<Policy>(*encoded_json);
    const auto toml_back = toml::deserialize<Policy>(*encoded_toml);
    assert(json_back && *json_back == valid);
    assert(toml_back && *toml_back == valid);

    std::string streamed;
    json::stream::StreamWriter writer{
        [&streamed](std::string_view part) -> json::result<void> {
            streamed.append(part);
            return {};
        }};
    const auto stream_result = writer.value(valid);
    assert(stream_result);
    const auto finished = writer.finish();
    assert(finished);
    const auto streamed_back = json::deserialize<Policy>(streamed);
    assert(streamed_back && *streamed_back == valid);

    const auto invalid_json = json::deserialize<Policy>(
        R"({"display-name":"Ada","quota":0,"ports":[80],"mode":"active"})");
    assert(!invalid_json && invalid_json.error().find("quota") != std::string::npos);
    const auto invalid_toml = toml::deserialize<Policy>(
        "display-name = \"Ada\"\nquota = 0\nports = [80]\nmode = \"active\"\n");
    assert(!invalid_toml && invalid_toml.error().find("quota") != std::string::npos);

    Policy invalid = valid;
    invalid.name = "too-long";
    const auto rejected_json = json::serialize(invalid);
    const auto rejected_toml = toml::serialize(invalid);
    assert(!rejected_json && !rejected_toml);
    std::string discarded;
    json::stream::StreamWriter failing_writer{
        [&discarded](std::string_view part) -> json::result<void> {
            discarded.append(part);
            return {};
        }};
    const auto rejected_stream = failing_writer.value(invalid);
    assert(!rejected_stream);
    const auto rejected_finish = failing_writer.finish();
    assert(!rejected_finish && rejected_finish.error() == rejected_stream.error());

    const auto invalid_enum = json::deserialize<Policy>(
        R"({"display-name":"Ada","quota":10,"ports":[80],"mode":"unknown"})");
    assert(!invalid_enum);
    invalid = valid;
    invalid.mode = static_cast<Mode>(99);
    const auto enum_json = json::serialize(invalid);
    const auto enum_toml = toml::serialize(invalid);
    assert(!enum_json && !enum_toml);

    auto partial = json::parse(
        R"({"display-name":"Bob","quota":20,"ports":[8080],"mode":"disabled"})");
    assert(partial);
    Policy merged = valid;
    const auto merged_result = json::decode_fields_into(
        *partial, merged, [](const auto& field) { return field.name != "retries"; });
    assert(merged_result && merged.retries == 3 && merged.name == "Bob");
    assert(merged.quota == 20 && merged.mode == Mode::Disabled);
    auto bad_partial = json::parse(R"({"quota":101})");
    assert(bad_partial);
    const auto failed_merge = json::decode_fields_into(
        *bad_partial, merged, [](const auto& field) { return field.name == "quota"; });
    assert(!failed_merge);
    std::cout << "serde consumer contract passed\n";
}
