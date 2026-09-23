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

#define WIDE_FIELDS_01_16(X) \
    X(f001) \
    X(f002) \
    X(f003) \
    X(f004) \
    X(f005) \
    X(f006) \
    X(f007) \
    X(f008) \
    X(f009) \
    X(f010) \
    X(f011) \
    X(f012) \
    X(f013) \
    X(f014) \
    X(f015) \
    X(f016)
#define WIDE_FIELDS_17_32(X) \
    X(f017) \
    X(f018) \
    X(f019) \
    X(f020) \
    X(f021) \
    X(f022) \
    X(f023) \
    X(f024) \
    X(f025) \
    X(f026) \
    X(f027) \
    X(f028) \
    X(f029) \
    X(f030) \
    X(f031) \
    X(f032)
#define WIDE_FIELDS_33_64(X) \
    X(f033) \
    X(f034) \
    X(f035) \
    X(f036) \
    X(f037) \
    X(f038) \
    X(f039) \
    X(f040) \
    X(f041) \
    X(f042) \
    X(f043) \
    X(f044) \
    X(f045) \
    X(f046) \
    X(f047) \
    X(f048) \
    X(f049) \
    X(f050) \
    X(f051) \
    X(f052) \
    X(f053) \
    X(f054) \
    X(f055) \
    X(f056) \
    X(f057) \
    X(f058) \
    X(f059) \
    X(f060) \
    X(f061) \
    X(f062) \
    X(f063) \
    X(f064)
#define WIDE_FIELDS_65_128(X) \
    X(f065) \
    X(f066) \
    X(f067) \
    X(f068) \
    X(f069) \
    X(f070) \
    X(f071) \
    X(f072) \
    X(f073) \
    X(f074) \
    X(f075) \
    X(f076) \
    X(f077) \
    X(f078) \
    X(f079) \
    X(f080) \
    X(f081) \
    X(f082) \
    X(f083) \
    X(f084) \
    X(f085) \
    X(f086) \
    X(f087) \
    X(f088) \
    X(f089) \
    X(f090) \
    X(f091) \
    X(f092) \
    X(f093) \
    X(f094) \
    X(f095) \
    X(f096) \
    X(f097) \
    X(f098) \
    X(f099) \
    X(f100) \
    X(f101) \
    X(f102) \
    X(f103) \
    X(f104) \
    X(f105) \
    X(f106) \
    X(f107) \
    X(f108) \
    X(f109) \
    X(f110) \
    X(f111) \
    X(f112) \
    X(f113) \
    X(f114) \
    X(f115) \
    X(f116) \
    X(f117) \
    X(f118) \
    X(f119) \
    X(f120) \
    X(f121) \
    X(f122) \
    X(f123) \
    X(f124) \
    X(f125) \
    X(f126) \
    X(f127) \
    X(f128)
#define WIDE_FIELDS_17(X) WIDE_FIELDS_01_16(X) X(f017)
#define WIDE_FIELDS_32(X) WIDE_FIELDS_01_16(X) WIDE_FIELDS_17_32(X)
#define WIDE_FIELDS_64(X) WIDE_FIELDS_01_16(X) WIDE_FIELDS_17_32(X) WIDE_FIELDS_33_64(X)
#define WIDE_FIELDS_128(X) WIDE_FIELDS_01_16(X) WIDE_FIELDS_17_32(X) WIDE_FIELDS_33_64(X) WIDE_FIELDS_65_128(X)

#define WIDE_DECLARE(name) std::int64_t name{};
struct Wide17 { WIDE_FIELDS_17(WIDE_DECLARE) };
struct Wide32 { WIDE_FIELDS_32(WIDE_DECLARE) };
struct Wide64 { WIDE_FIELDS_64(WIDE_DECLARE) };
struct Wide128 { WIDE_FIELDS_128(WIDE_DECLARE) };
#undef WIDE_DECLARE

REFLECT_FIELDS(Wide17, WIDE_FIELDS_17)
REFLECT_FIELDS(Wide32, WIDE_FIELDS_32)
REFLECT_FIELDS(Wide64, WIDE_FIELDS_64)
REFLECT_FIELDS(Wide128, WIDE_FIELDS_128)

#undef WIDE_FIELDS_01_16
#undef WIDE_FIELDS_17_32
#undef WIDE_FIELDS_33_64
#undef WIDE_FIELDS_65_128
#undef WIDE_FIELDS_17
#undef WIDE_FIELDS_32
#undef WIDE_FIELDS_64
#undef WIDE_FIELDS_128

template <class T, std::size_t Expected>
bool round_trip(std::string_view label) {
    T original{};
    std::size_t written = 0;
    reflect::for_each_field(original, [&](const auto& descriptor, auto& object) {
        descriptor.get(object) = static_cast<std::int64_t>(++written);
    });
    bool passed = written == Expected;
    const auto serialized = toml::serialize(original);
    if (!serialized) {
        std::println("test_wide_fields: {} serialize failed: {}", label, serialized.error());
        return false;
    }
    const auto parsed = toml::deserialize<T>(*serialized);
    if (!parsed) {
        std::println("test_wide_fields: {} deserialize failed: {}", label, parsed.error());
        return false;
    }
    std::size_t read = 0;
    std::apply([&](const auto&... descriptor) {
        ((passed = passed && descriptor.get(*parsed) ==
                              static_cast<std::int64_t>(++read)), ...);
    }, reflect::fields(*parsed));
    if (!passed || read != Expected) {
        std::println("test_wide_fields: {} lost fields during round-trip", label);
    }
    return passed && read == Expected;
}

static_assert(std::tuple_size_v<decltype(reflect::fields(std::declval<const Wide17&>()))> == 17);
static_assert(std::tuple_size_v<decltype(reflect::fields(std::declval<const Wide32&>()))> == 32);
static_assert(std::tuple_size_v<decltype(reflect::fields(std::declval<const Wide64&>()))> == 64);
static_assert(std::tuple_size_v<decltype(reflect::fields(std::declval<const Wide128&>()))> == 128);

int main() {
    bool passed = true;
    passed &= round_trip<Wide17, 17>("17 fields");
    passed &= round_trip<Wide32, 32>("32 fields");
    passed &= round_trip<Wide64, 64>("64 fields");
    passed &= round_trip<Wide128, 128>("128 fields");
    return passed ? 0 : 1;
}
