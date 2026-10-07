#include <serde/json/json.hpp>
#include <serde/reflect/reflect_macros.hpp>
#include <serde/toml/toml.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace example {

enum class Mode : std::uint8_t { Active, Disabled };

constexpr auto reflect_enum(std::type_identity<Mode>) {
    return reflect::enum_descriptor<Mode, 2>{
        reflect::enum_encoding::string,
        {{{Mode::Active, "active"}, {Mode::Disabled, "disabled"}}}
    };
}

struct Settings {
    std::string name;
    int attempts{};
    Mode mode = Mode::Active;
};

constexpr reflect::field_options<std::string> name_options{
    .description = "Application name", .min_length = 1, .max_length = 32};
constexpr reflect::field_options<int> attempts_options{.minimum = 1, .maximum = 5};

#define SETTINGS_FIELDS(X) \
    X(name, "name", (name_options)) \
    X(attempts, "attempts", (attempts_options)) \
    X(mode)
REFLECT_FIELDS(Settings, SETTINGS_FIELDS)
#undef SETTINGS_FIELDS

} // namespace example

int main() {
    const example::Settings settings{"galay", 3, example::Mode::Active};
    const auto encoded_json = json::serialize(settings);
    if (!encoded_json) {
        std::cerr << encoded_json.error() << '\n';
        return 1;
    }
    const auto encoded_toml = toml::serialize(settings);
    if (!encoded_toml) {
        std::cerr << encoded_toml.error() << '\n';
        return 1;
    }
    std::cout << *encoded_json << '\n' << *encoded_toml;
    const auto rejected = json::deserialize<example::Settings>(
        R"({"name":"galay","attempts":6,"mode":"active"})");
    if (rejected) {
        std::cerr << "invalid attempts were accepted\n";
        return 1;
    }
    std::cout << "Rejected: " << rejected.error() << '\n';
    return 0;
}
