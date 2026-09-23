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
#include <serde/reflect/reflect.hpp>

#include <serde/reflect/reflect_macros.hpp>

namespace sample {

struct settings {
    std::string host;
    int port{};
};

#define SERDE_FIELDS_12(X) \
    X(host) \
    X(port)
REFLECT_FIELDS(settings, SERDE_FIELDS_12)
#undef SERDE_FIELDS_12

struct renamed {
    int retry_count{};
};

#define RENAMED_FIELDS(X) X(retry_count, "retry-count")
REFLECT_FIELDS(renamed, RENAMED_FIELDS)
#undef RENAMED_FIELDS

struct runtime_named {
    int value{};
};

std::string field_name = "first";
#define RUNTIME_FIELDS(X) X(value, field_name)
REFLECT_FIELDS(runtime_named, RUNTIME_FIELDS)
#undef RUNTIME_FIELDS

struct mutable_name_view {
    int value{};
};

char mutable_name[] = "first";
constexpr std::string_view name_view{mutable_name, 5};
#define MUTABLE_VIEW_FIELDS(X) X(value, name_view)
REFLECT_FIELDS(mutable_name_view, MUTABLE_VIEW_FIELDS)
#undef MUTABLE_VIEW_FIELDS

struct custom_fields {
    int value{};
};

auto reflect_fields(const custom_fields&) {
    return std::tuple{reflect::make_field(field_name, &custom_fields::value)};
}

struct empty {};
REFLECT_EMPTY(empty)

}  // namespace sample

int main() {
    static_assert(reflect::Reflectable<sample::settings>);
    static_assert(reflect::StaticReflectable<sample::settings>);
    static_assert(reflect::StaticReflectable<sample::renamed>);
    static_assert(reflect::StaticReflectable<sample::empty>);
    static_assert(std::get<0>(reflect::static_fields<sample::renamed>()).name == "retry-count");
    static_assert(!reflect::StaticReflectable<sample::runtime_named>);
    static_assert(!reflect::StaticReflectable<sample::mutable_name_view>);
    static_assert(!reflect::StaticReflectable<sample::custom_fields>);
    static_assert(!reflect::StaticReflectable<int>);
    sample::settings value{"localhost", 8080};
    std::vector<std::string> names;
    reflect::for_each_field(value, [&](const auto& descriptor, auto&) {
        names.emplace_back(descriptor.name);
    });
    if (names != std::vector<std::string>{"host", "port"}) {
        std::println("test_reflect: field names are not stable");
        return 1;
    }

    reflect::for_each_field(value, [&](const auto& descriptor, auto& object) {
        if constexpr (std::same_as<std::remove_cvref_t<decltype(descriptor.get(object))>, int>) {
            if (descriptor.name == "port") {
                descriptor.get(object) = 9090;
            }
        }
    });
    if (value.port != 9090) {
        std::println("test_reflect: member pointer access failed");
        return 1;
    }

    sample::renamed renamed{3};
    const auto fields = reflect::fields(renamed);
    if (std::get<0>(fields).name != "retry-count") {
        std::println("test_reflect: named field mapping failed");
        return 1;
    }
    sample::field_name = "second";
    if (std::get<0>(reflect::fields(sample::runtime_named{})).name != "second" ||
        std::get<0>(reflect::fields(sample::custom_fields{})).name != "second") {
        std::println("test_reflect: runtime field mapping was cached");
        return 1;
    }
    return 0;
}
