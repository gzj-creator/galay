#ifndef GALAY_API_SCHEMA_H
#define GALAY_API_SCHEMA_H

#include "schema_model.h"
#include <serde/reflect/reflect.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace galay::api {
namespace schema_detail {

inline ApiError error(ApiErrorCode code, std::string message) {
    return {code, std::move(message), 500};
}

template<class T>
struct MetadataProbe { std::optional<T> value; };

inline ApiResult<void> validate_text(std::string_view text,
                                    ApiErrorCode code = ApiErrorCode::kInvalidMetadata) {
    const auto descriptor = reflect::make_field("text", &MetadataProbe<int>::value,
        reflect::field_options<std::optional<int>>{.description = text});
    if (auto checked = reflect::validate_field(descriptor, std::optional<int>{}); !checked) {
        return std::unexpected(error(code, std::move(checked.error())));
    }
    return {};
}

template<class T>
struct UnwrapOptional { using type = T; };
template<class T>
struct UnwrapOptional<std::optional<T>> : UnwrapOptional<T> {};
template<class T>
using Unwrapped = typename UnwrapOptional<std::remove_cvref_t<T>>::type;

template<class T>
struct Sequence { static constexpr bool value = false; };
template<class T, class Allocator>
struct Sequence<std::vector<T, Allocator>> {
    static constexpr bool value = true;
    static constexpr bool fixed = false;
    using element = T;
};
template<class T, std::size_t N>
struct Sequence<std::array<T, N>> {
    static constexpr bool value = true;
    static constexpr bool fixed = true;
    static constexpr std::size_t size = N;
    using element = T;
};

template<class T>
struct StringMap { static constexpr bool value = false; };
template<class V, class Compare, class Allocator>
struct StringMap<std::map<std::string, V, Compare, Allocator>> {
    static constexpr bool value = true;
    using mapped = V;
};
template<class V, class Hash, class Equal, class Allocator>
struct StringMap<std::unordered_map<std::string, V, Hash, Equal, Allocator>> {
    static constexpr bool value = true;
    using mapped = V;
};

template<class T>
struct MemberPointer;
template<class Owner, class Member>
struct MemberPointer<Member Owner::*> { using type = Member; };

template<class T>
SchemaNumber number(T value) {
    if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) return std::int64_t(value);
    else if constexpr (std::is_integral_v<T>) return std::uint64_t(value);
    else return double(value);
}

template<class T>
constexpr std::size_t enum_checksum() {
    const auto descriptor = reflect::enum_descriptor_for<T>();
    std::size_t result = static_cast<std::size_t>(descriptor.encoding);
    for (const auto& entry : descriptor.values) {
        result += static_cast<std::size_t>(entry.value);
        for (unsigned char ch : entry.name) result += ch;
    }
    return result;
}

template<class T>
concept StaticEnum = reflect::EnumReflectable<T> && requires {
    typename std::integral_constant<std::size_t, enum_checksum<T>()>;
};

template<class T>
ApiResult<void> enum_contract() {
    if constexpr (reflect::EnumReflectable<T>) {
        if constexpr (!StaticEnum<T>) {
            return std::unexpected(error(ApiErrorCode::kUnsupportedType, "enum documentation requires a static descriptor"));
        } else {
            if (auto checked = reflect::validate_enum_descriptor<T>(); !checked) {
                return std::unexpected(error(ApiErrorCode::kInvalidMetadata, std::move(checked.error())));
            }
            constexpr auto stable = reflect::enum_descriptor_for<T>();
            const auto current = reflect::enum_descriptor_for<T>();
            if (current.encoding != stable.encoding) {
                return std::unexpected(error(ApiErrorCode::kInvalidMetadata, "enum encoding changed from its static contract"));
            }
            for (std::size_t i = 0; i < stable.values.size(); ++i) {
                if (current.values[i].value != stable.values[i].value || current.values[i].name != stable.values[i].name) {
                    return std::unexpected(error(ApiErrorCode::kInvalidMetadata, "enum values changed from their static contract"));
                }
            }
        }
    }
    return {};
}

template<class Static, class Runtime>
bool same_field(const Static& stable, const Runtime& current) {
    if constexpr (requires { stable.pointer == current.pointer; stable.name; current.name; }) {
        if (stable.pointer == nullptr || current.pointer == nullptr ||
            stable.name != current.name || stable.pointer != current.pointer) return false;
        if constexpr (requires { stable.options; current.options; }) {
            return stable.options.description == current.options.description &&
                stable.options.minimum == current.options.minimum && stable.options.maximum == current.options.maximum &&
                stable.options.min_length == current.options.min_length && stable.options.max_length == current.options.max_length &&
                stable.options.min_items == current.options.min_items && stable.options.max_items == current.options.max_items;
        } else return !(requires { stable.options; }) && !(requires { current.options; });
    }
    return false;
}

template<class Static, class Runtime>
bool same_fields(const Static& stable, const Runtime& current) {
    if constexpr (std::tuple_size_v<Static> != std::tuple_size_v<Runtime>) return false;
    else {
        return [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            return (same_field(std::get<Index>(stable), std::get<Index>(current)) && ...);
        }(std::make_index_sequence<std::tuple_size_v<Static>>{});
    }
}

// Check descriptor stability, not values or codecs. This never constructs a schema.
template<class T, class... Seen>
ApiResult<void> contract(const T* value) {
    using U = std::remove_cvref_t<T>;
    if constexpr ((std::same_as<U, Seen> || ...) || sizeof...(Seen) >= 128) {
        return std::unexpected(error(ApiErrorCode::kInvalidSchema, "recursive contract is unsupported"));
    } else if constexpr (reflect::is_optional_v<U>) {
        using Member = typename reflect::is_optional<U>::value_type;
        return contract<Member, Seen..., U>(value && *value ? std::addressof(**value) : nullptr);
    } else if constexpr (std::is_enum_v<U>) {
        return enum_contract<U>();
    } else if constexpr (Sequence<U>::value) {
        using Member = typename Sequence<U>::element;
        if (auto checked = contract<Member, Seen..., U>(nullptr); !checked) return checked;
        if constexpr (!std::same_as<Member, bool>) {
            if (value) for (const auto& item : *value) {
                if (auto checked = contract<Member, Seen..., U>(std::addressof(item)); !checked) return checked;
            }
        }
    } else if constexpr (StringMap<U>::value) {
        using Member = typename StringMap<U>::mapped;
        if (auto checked = contract<Member, Seen..., U>(nullptr); !checked) return checked;
        if (value) for (const auto& entry : *value) {
            if (auto checked = contract<Member, Seen..., U>(std::addressof(entry.second)); !checked) return checked;
        }
    } else if constexpr (reflect::StaticReflectable<U>) {
        if constexpr (std::is_default_constructible_v<U>) {
            if (!value) {
                const U probe{};
                return contract<U, Seen...>(std::addressof(probe));
            }
        }
        constexpr auto stable = reflect::static_fields<U>();
        const auto current = reflect::static_fields<U>();
        if (!same_fields(stable, current) || (value && !same_fields(stable, reflect::fields(*value)))) {
            return std::unexpected(error(ApiErrorCode::kInvalidMetadata,
                "runtime field name, member or options differ from the static serde contract"));
        }
        ApiResult<void> status;
        std::apply([&](const auto&... field) {
            const auto check = [&](const auto& descriptor) {
                if (!status) return;
                using Member = std::remove_cvref_t<decltype(descriptor.get(std::declval<const U&>()))>;
                status = contract<Member, Seen..., U>(value ? std::addressof(descriptor.get(*value)) : nullptr);
            };
            (check(field), ...);
        }, stable);
        return status;
    }
    return {};
}

template<class Member, class Descriptor>
ApiResult<void> validate_metadata(const Descriptor& descriptor) {
    if constexpr (requires { descriptor.pointer; }) {
        if (descriptor.pointer == nullptr) {
            return std::unexpected(error(ApiErrorCode::kInvalidMetadata, "field descriptor has a null member pointer"));
        }
    }
    if constexpr (requires { descriptor.options; }) {
        const auto& source = descriptor.options;
        // An empty optional runs serde's metadata checks without inventing a member value.
        const reflect::field_options<std::optional<Member>> options{
            .description = source.description, .minimum = source.minimum, .maximum = source.maximum,
            .min_length = source.min_length, .max_length = source.max_length,
            .min_items = source.min_items, .max_items = source.max_items};
        const auto probe = reflect::make_field(descriptor.name, &MetadataProbe<Member>::value, options);
        if (auto checked = reflect::validate_field(probe, std::optional<Member>{}); !checked) {
            return std::unexpected(error(ApiErrorCode::kInvalidMetadata,
                "field '" + std::string(descriptor.name) + "': " + checked.error()));
        }
    }
    return validate_text(descriptor.name);
}

template<class T, class... Seen>
ApiResult<Schema> build(SchemaUse use);

template<class Member, class... Seen, class Descriptor>
ApiResult<Schema> build_field(const Descriptor& descriptor, SchemaUse use) {
    if (auto checked = validate_metadata<Member>(descriptor); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    auto schema = build<Member, Seen...>(use);
    if (!schema) return schema;
    if constexpr (requires { descriptor.options; }) {
        const auto& options = descriptor.options;
        schema->description = options.description;
        if (options.minimum) schema->minimum = number(*options.minimum);
        if (options.maximum) schema->maximum = number(*options.maximum);
        schema->min_length = options.min_length;
        schema->max_length = options.max_length;
        using Value = Unwrapped<Member>;
        if constexpr (Sequence<Value>::value) {
            if constexpr (Sequence<Value>::fixed) {
                if ((options.min_items && *options.min_items > Sequence<Value>::size) ||
                    (options.max_items && *options.max_items < Sequence<Value>::size)) {
                    return std::unexpected(error(ApiErrorCode::kInvalidMetadata,
                        "field '" + std::string(descriptor.name) + "': fixed array length contradicts item bounds"));
                }
            } else {
                schema->min_items = options.min_items;
                schema->max_items = options.max_items;
            }
        } else if constexpr (StringMap<Value>::value) {
            schema->min_properties = options.min_items;
            schema->max_properties = options.max_items;
        }
        if constexpr (std::is_enum_v<Value>) {
            if (options.minimum || options.maximum) {
                const auto removed = std::erase_if(schema->enum_values, [&](const SchemaValue& value) {
                    return std::visit([&](const auto& numeric) {
                        using N = std::remove_cvref_t<decltype(numeric)>;
                        if constexpr (std::is_arithmetic_v<N> && !std::same_as<N, bool>) {
                            return (options.minimum && numeric < *options.minimum) ||
                                   (options.maximum && numeric > *options.maximum);
                        }
                        return false;
                    }, value);
                });
                if (removed != 0 && schema->enum_values.empty()) {
                    return std::unexpected(error(ApiErrorCode::kInvalidMetadata,
                        "field '" + std::string(descriptor.name) + "': constraints exclude every registered enum value"));
                }
            }
        }
    }
    return schema;
}

template<class T, class... Seen>
ApiResult<Schema> build(SchemaUse use) {
    using U = std::remove_cvref_t<T>;
    if (use != SchemaUse::input && use != SchemaUse::output) {
        return std::unexpected(error(ApiErrorCode::kInvalidMetadata, "invalid schema use"));
    }
    if constexpr ((std::same_as<U, Seen> || ...)) {
        return std::unexpected(error(ApiErrorCode::kInvalidSchema, "recursive type cannot be represented by an inline schema"));
    } else if constexpr (sizeof...(Seen) >= 128) {
        return std::unexpected(error(ApiErrorCode::kInvalidSchema, "inline schema nesting exceeds 128 types"));
    } else if constexpr (reflect::is_optional_v<U>) {
        auto schema = build<typename reflect::is_optional<U>::value_type, Seen..., U>(use);
        if (schema) schema->nullable = true;
        return schema;
    } else if constexpr (std::same_as<U, bool>) {
        Schema schema;
        schema.type = "boolean";
        return schema;
    } else if constexpr (std::is_integral_v<U>) {
        if constexpr (std::numeric_limits<U>::digits > (std::is_signed_v<U> ? 63 : 64)) {
            return std::unexpected(error(ApiErrorCode::kUnsupportedType, "integers wider than the JSON 64-bit range are unsupported"));
        } else {
            Schema schema;
            schema.type = "integer";
            if constexpr (std::is_signed_v<U>) schema.format = sizeof(U) <= 4 ? "int32" : "int64";
            schema.minimum = number(std::numeric_limits<U>::min());
            schema.maximum = number(std::numeric_limits<U>::max());
            return schema;
        }
    } else if constexpr (std::same_as<U, float> || std::same_as<U, double>) {
        Schema schema;
        schema.type = "number";
        schema.format = std::same_as<U, float> ? "float" : "double";
        schema.minimum = number(std::numeric_limits<U>::lowest());
        schema.maximum = number(std::numeric_limits<U>::max());
        return schema;
    } else if constexpr (std::same_as<U, std::string> || std::same_as<U, std::string_view>) {
        if constexpr (std::same_as<U, std::string_view>) {
            if (use == SchemaUse::input) {
                return std::unexpected(error(ApiErrorCode::kUnsupportedType, "serde cannot decode an owning input into string_view"));
            }
        }
        Schema schema;
        schema.type = "string";
        return schema;
    } else if constexpr (std::is_enum_v<U>) {
        if constexpr (reflect::EnumReflectable<U>) {
            if constexpr (!StaticEnum<U>) {
                return std::unexpected(error(ApiErrorCode::kUnsupportedType, "enum documentation requires a static descriptor"));
            } else {
                if (auto checked = enum_contract<U>(); !checked) return std::unexpected(std::move(checked.error()));
                const auto descriptor = reflect::enum_descriptor_for<U>();
                Schema schema;
                if (descriptor.encoding == reflect::enum_encoding::string) {
                    schema.type = "string";
                    for (const auto& entry : descriptor.values) schema.enum_values.push_back(std::string(entry.name));
                } else {
                    auto underlying = build<std::underlying_type_t<U>, Seen..., U>(use);
                    if (!underlying) return underlying;
                    schema = std::move(*underlying);
                    for (const auto& entry : descriptor.values) {
                        using Underlying = std::underlying_type_t<U>;
                        if constexpr (std::same_as<Underlying, bool>) schema.enum_values.push_back(static_cast<bool>(entry.value));
                        else if constexpr (std::is_signed_v<Underlying>) schema.enum_values.push_back(static_cast<std::int64_t>(entry.value));
                        else schema.enum_values.push_back(static_cast<std::uint64_t>(entry.value));
                    }
                }
                return schema;
            }
        } else {
            return build<std::underlying_type_t<U>, Seen..., U>(use);
        }
    } else if constexpr (Sequence<U>::value) {
        auto items = build<typename Sequence<U>::element, Seen..., U>(use);
        if (!items) return items;
        Schema schema;
        schema.type = "array";
        schema.items = std::make_shared<Schema>(std::move(*items));
        if constexpr (Sequence<U>::fixed) {
            schema.min_items = Sequence<U>::size;
            schema.max_items = Sequence<U>::size;
        }
        return schema;
    } else if constexpr (StringMap<U>::value) {
        auto mapped = build<typename StringMap<U>::mapped, Seen..., U>(use);
        if (!mapped) return mapped;
        Schema schema;
        schema.type = "object";
        schema.additional_properties = std::make_shared<Schema>(std::move(*mapped));
        return schema;
    } else if constexpr (reflect::StaticReflectable<U>) {
        constexpr auto stable = reflect::static_fields<U>();
        const auto descriptors = reflect::static_fields<U>();
        Schema schema;
        schema.type = "object";
        schema.allow_additional_properties = use == SchemaUse::input;
        ApiResult<void> status;
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            auto add = [&]<std::size_t I>() {
                if (!status) return;
                const auto& descriptor = std::get<I>(descriptors);
                if (!same_field(std::get<I>(stable), descriptor)) {
                    status = std::unexpected(error(ApiErrorCode::kInvalidMetadata,
                        "DTO field name, member or options changed from their static contract"));
                    return;
                }
                using Member = std::remove_cvref_t<decltype(descriptor.get(std::declval<U&>()))>;
                auto property = build_field<Member, Seen..., U>(descriptor, use);
                if (!property) {
                    status = std::unexpected(std::move(property.error()));
                    return;
                }
                const auto [position, inserted] = schema.properties.emplace(std::string(descriptor.name), std::move(*property));
                if (!inserted) {
                    status = std::unexpected(error(ApiErrorCode::kInvalidMetadata, "duplicate DTO field name: " + position->first));
                    return;
                }
                if (use == SchemaUse::output || !reflect::is_optional_v<Member>) schema.required.push_back(position->first);
            };
            (add.template operator()<Index>(), ...);
        }(std::make_index_sequence<std::tuple_size_v<decltype(descriptors)>>{});
        if (!status) return std::unexpected(std::move(status.error()));
        std::sort(schema.required.begin(), schema.required.end());
        return schema;
    } else {
        return std::unexpected(error(ApiErrorCode::kUnsupportedType, "type has no supported static serde JSON contract"));
    }
}

} // namespace schema_detail

template<class T>
ApiResult<Schema> schema_for(SchemaUse use) {
    auto schema = schema_detail::build<T>(use);
    if (!schema) return schema;
    if constexpr (std::is_default_constructible_v<T>) {
        const T probe{};
        if (auto checked = schema_detail::contract<T>(std::addressof(probe)); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
    }
    return schema;
}

template<class T>
ApiResult<void> validate_contract(const T& value) {
    return schema_detail::contract<T>(std::addressof(value));
}

template<class Descriptor>
ApiResult<Schema> schema_for_field(const Descriptor& descriptor, SchemaUse use) {
    if constexpr (requires { descriptor.pointer; descriptor.name; }) {
        using Pointer = std::remove_cvref_t<decltype(descriptor.pointer)>;
        if constexpr (std::is_member_object_pointer_v<Pointer>) {
            using Member = typename schema_detail::MemberPointer<Pointer>::type;
            return schema_detail::build_field<Member>(descriptor, use);
        }
    }
    return std::unexpected(schema_detail::error(ApiErrorCode::kUnsupportedType,
        "a field schema requires a public serde member-object descriptor"));
}

} // namespace galay::api

#endif
