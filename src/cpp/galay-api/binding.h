#ifndef GALAY_API_BINDING_H
#define GALAY_API_BINDING_H

#include <galay/cpp/galay-http/server/api_binding.h>
#if defined(__APPLE__) && defined(__MACH__)
#include <galay/thirdparty/fast_float/include/fast_float/fast_float.h>
#endif
#include "schema.h"
#include <serde/json/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <tuple>
#include <type_traits>

namespace galay::api {
namespace binding_detail {

inline ApiError invalid(std::string message) {
    return {ApiErrorCode::kInvalidBinding, std::move(message), 500};
}

inline ApiError bad_request(std::string message) {
    return {ApiErrorCode::kBadRequest, std::move(message), 400};
}

inline bool valid_name(std::string_view name) {
    return !name.empty() && simdjson::validate_utf8(name) &&
        std::none_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

template<class T>
constexpr bool scalar() {
    using U = std::remove_cvref_t<T>;
    if constexpr (reflect::is_optional_v<U>) return scalar<typename reflect::is_optional<U>::value_type>();
    else return std::same_as<U, std::string> || std::is_integral_v<U> ||
        std::same_as<U, float> || std::same_as<U, double> || std::is_enum_v<U>;
}

template<class T>
ApiResult<T> decode_scalar(std::string_view text) {
    if (!simdjson::validate_utf8(text)) {
        return std::unexpected(bad_request("parameter is not valid UTF-8"));
    }
    if constexpr (reflect::is_optional_v<T>) {
        auto value = decode_scalar<typename reflect::is_optional<T>::value_type>(text);
        if (!value) return std::unexpected(std::move(value.error()));
        return T{std::move(*value)};
    } else if constexpr (std::same_as<T, std::string>) {
        return std::string(text);
    } else if constexpr (std::same_as<T, bool>) {
        if (text == "true") return true;
        if (text == "false") return false;
        return std::unexpected(bad_request("boolean parameter must be 'true' or 'false'"));
    } else if constexpr (std::is_enum_v<T>) {
        if constexpr (reflect::EnumReflectable<T>) {
            const auto descriptor = reflect::enum_descriptor_for<T>();
            if (descriptor.encoding == reflect::enum_encoding::string) {
                auto value = reflect::enum_from_string<T>(text);
                if (!value) return std::unexpected(bad_request(std::move(value.error())));
                return *value;
            }
        }
        auto underlying = decode_scalar<std::underlying_type_t<T>>(text);
        if (!underlying) return std::unexpected(std::move(underlying.error()));
        const auto value = static_cast<T>(*underlying);
        if (auto checked = reflect::validate_enum(value); !checked) {
            return std::unexpected(bad_request(std::move(checked.error())));
        }
        return value;
    } else if constexpr (std::is_integral_v<T> || std::same_as<T, float> || std::same_as<T, double>) {
        if (text.empty()) return std::unexpected(bad_request("numeric parameter is empty"));
        using Parsed = std::conditional_t<std::is_integral_v<T>,
            std::conditional_t<std::is_signed_v<T>, std::int64_t, std::uint64_t>, T>;
        Parsed value{};
        const std::from_chars_result converted = [&] {
#if defined(__APPLE__) && defined(__MACH__)
            if constexpr (std::is_floating_point_v<T>) {
                const auto result = fast_float::from_chars(text.data(), text.data() + text.size(), value);
                return std::from_chars_result{result.ptr, result.ec};
            } else {
                return std::from_chars(text.data(), text.data() + text.size(), value);
            }
#else
            return std::from_chars(text.data(), text.data() + text.size(), value);
#endif
        }();
        if (converted.ec == std::errc::result_out_of_range) {
            return std::unexpected(bad_request("numeric parameter is outside the field's range"));
        }
        if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size()) {
            return std::unexpected(bad_request("numeric parameter must be consumed completely"));
        }
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(value)) return std::unexpected(bad_request("numeric parameter must be finite"));
        } else {
            if (value < static_cast<Parsed>(std::numeric_limits<T>::min()) ||
                value > static_cast<Parsed>(std::numeric_limits<T>::max())) {
                return std::unexpected(bad_request("numeric parameter is outside the field's range"));
            }
        }
        return static_cast<T>(value);
    } else {
        return std::unexpected(invalid("path/query field is not a supported scalar"));
    }
}

template<class Descriptor, class Value>
ApiResult<void> validate_member(const Descriptor& descriptor, const Value& value, bool retained_default) {
    if (auto checked = reflect::validate_field(descriptor, value); !checked) {
        return std::unexpected(bad_request("field '" + std::string(descriptor.name) + "': " + checked.error()));
    }
    if (retained_default) {
        // Defaults can contain nested enums, non-finite numbers or invalid UTF-8.
        const auto encoded = json::serialize(value);
        if (!encoded) {
            return std::unexpected(bad_request("default for field '" + std::string(descriptor.name) + "': " + encoded.error()));
        }
    }
    return {};
}

inline bool json_media_type(std::string_view type) {
    // HeaderPair preserves repeated common headers as a comma-separated value.
    // A request with more than one Content-Type is not an unambiguous JSON body.
    if (type.find(',') != std::string_view::npos) return false;
    type = type.substr(0, type.find(';'));
    while (!type.empty() && (type.front() == ' ' || type.front() == '\t')) type.remove_prefix(1);
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) type.remove_suffix(1);
    constexpr std::string_view expected = "application/json";
    if (type.size() != expected.size()) return false;
    for (std::size_t i = 0; i < type.size(); ++i) {
        const auto ch = static_cast<unsigned char>(type[i]);
        const auto lower = ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
        if (lower != static_cast<unsigned char>(expected[i])) return false;
    }
    return true;
}

inline Schema strict_body(Schema schema) {
    if (schema.type == "object" && !schema.additional_properties) schema.allow_additional_properties = false;
    for (auto& [name, property] : schema.properties) property = strict_body(std::move(property));
    if (schema.items) schema.items = std::make_shared<Schema>(strict_body(*schema.items));
    if (schema.additional_properties) {
        schema.additional_properties = std::make_shared<Schema>(strict_body(*schema.additional_properties));
    }
    return schema;
}

template<class Input>
ApiResult<void> stable_descriptors(const Input& input) {
    if (auto checked = validate_contract(input); !checked) {
        return std::unexpected(invalid(std::move(checked.error().message)));
    }
    return {};
}

template<class Input>
ApiResult<void> unique_members() {
    const auto descriptors = reflect::static_fields<Input>();
    ApiResult<void> status;
    std::size_t first_index = 0;
    const auto compare_one = [&](const auto& first) {
        std::size_t second_index = 0;
        std::apply([&](const auto&... second) {
            ([&] {
                if constexpr (requires { first.pointer == second.pointer; }) {
                    if (status && second_index < first_index && first.pointer == second.pointer) {
                        status = std::unexpected(invalid("a DTO member is reflected more than once"));
                    }
                }
                ++second_index;
            }(), ...);
        }, descriptors);
        ++first_index;
    };
    std::apply([&](const auto&... first) { (compare_one(first), ...); }, descriptors);
    return status;
}

} // namespace binding_detail

template<class Input>
template<auto Member>
InputBinding<Input>& InputBinding<Input>::path(std::string name) {
    return add<Member>(ParameterSource::path, std::move(name));
}

template<class Input>
template<auto Member>
InputBinding<Input>& InputBinding<Input>::query(std::string name) {
    return add<Member>(ParameterSource::query, std::move(name));
}

template<class Input>
template<auto Member>
InputBinding<Input>& InputBinding<Input>::add(ParameterSource source, std::string name) {
    if (error_) return *this;
    if (source != ParameterSource::path && source != ParameterSource::query) {
        error_ = binding_detail::invalid("unknown parameter source");
    } else if (!binding_detail::valid_name(name)) {
        error_ = binding_detail::invalid("parameter name must be nonempty UTF-8 without control characters");
    } else if constexpr (!reflect::StaticReflectable<Input> ||
                         !std::is_member_object_pointer_v<decltype(Member)> ||
                         !requires(Input& input) { input.*Member; }) {
        error_ = binding_detail::invalid("parameter must refer to a static input DTO member");
    } else {
        bool found = false;
        const auto descriptors = reflect::static_fields<Input>();
        std::apply([&](const auto&... descriptor) {
            ([&] {
                if constexpr (requires { descriptor.pointer == Member; }) {
                    if (descriptor.pointer != Member) return;
                    if (found) {
                        error_ = binding_detail::invalid("a DTO member is reflected more than once");
                        return;
                    }
                    found = true;
                    using Value = std::remove_cvref_t<decltype(descriptor.get(std::declval<Input&>()))>;
                    if constexpr (!binding_detail::scalar<Value>() ||
                                  !std::is_assignable_v<decltype(descriptor.get(std::declval<Input&>())), Value>) {
                        error_ = binding_detail::invalid("path/query member must be an assignable scalar");
                    } else {
                        const std::string field_name(descriptor.name);
                        for (const auto& previous : members_) {
                            if (previous.field_name == field_name ||
                                (previous.source == source && previous.name == name)) {
                                error_ = binding_detail::invalid("duplicate DTO member or parameter source");
                                return;
                            }
                        }
                        if (source == ParameterSource::path && reflect::is_optional_v<Value>) {
                            error_ = binding_detail::invalid("path parameters cannot be optional");
                            return;
                        }
                        members_.push_back(MemberBinding{
                            source, name, field_name,
                            [descriptor, source, name, field_name]() -> ApiResult<Parameter> {
                                auto schema = schema_for_field(descriptor, SchemaUse::input);
                                if (!schema) return std::unexpected(std::move(schema.error()));
                                // A textual parameter has no null token; optional means it may be absent.
                                schema->nullable = false;
                                return Parameter{source, name, field_name, schema->description,
                                    source == ParameterSource::path || !reflect::is_optional_v<Value>, std::move(*schema)};
                            },
                            [descriptor, name](Input& input, std::optional<std::string_view> text) -> ApiResult<void> {
                                if (!text) {
                                    if constexpr (reflect::is_optional_v<Value>) {
                                        return binding_detail::validate_member(descriptor, descriptor.get(input), true);
                                    }
                                    return std::unexpected(binding_detail::bad_request("missing parameter '" + name + "'"));
                                }
                                auto value = binding_detail::decode_scalar<Value>(*text);
                                if (!value) {
                                    value.error().message = "parameter '" + name + "': " + value.error().message;
                                    return std::unexpected(std::move(value.error()));
                                }
                                if (auto checked = binding_detail::validate_member(descriptor, *value, false); !checked) return checked;
                                descriptor.get(input) = std::move(*value);
                                return {};
                            }});
                    }
                }
            }(), ...);
        }, descriptors);
        if (!found) error_ = binding_detail::invalid("parameter member is absent from the static serde contract");
    }
    return *this;
}

template<class Input>
ApiResult<BindingPlan<Input>> InputBinding<Input>::prepare(http::HttpMethod method, std::string_view path) const {
    if (error_) return std::unexpected(*error_);
    switch (method) {
    case http::HttpMethod::GET: case http::HttpMethod::POST: case http::HttpMethod::HEAD:
    case http::HttpMethod::PUT: case http::HttpMethod::DELETE: case http::HttpMethod::PATCH:
    case http::HttpMethod::OPTIONS: case http::HttpMethod::TRACE: break;
    default: return std::unexpected(binding_detail::invalid("unsupported HTTP method for input binding"));
    }
    const auto inspected = inspect_path(path);
    if (!inspected) return std::unexpected(inspected.error());
    if constexpr (!std::is_default_constructible_v<Input> ||
                  (!std::same_as<Input, NoInput> && !reflect::StaticReflectable<Input>)) {
        return std::unexpected(binding_detail::invalid("input must be a default-constructible static DTO or NoInput"));
    } else {
        BindingPlan<Input> plan;
        std::vector<std::string> path_names;
        for (const auto& member : members_) {
            if (member.source != ParameterSource::path && member.source != ParameterSource::query) {
                return std::unexpected(binding_detail::invalid("unknown parameter source"));
            }
            auto parameter = member.describe();
            if (!parameter) return std::unexpected(std::move(parameter.error()));
            if (member.source == ParameterSource::path) path_names.push_back(member.name);
            plan.parameters.push_back(std::move(*parameter));
        }
        auto expected_names = inspected->parameters;
        std::sort(expected_names.begin(), expected_names.end());
        std::sort(path_names.begin(), path_names.end());
        if (expected_names != path_names) return std::unexpected(binding_detail::invalid("path parameters must each have exactly one matching member source"));

        if constexpr (reflect::StaticReflectable<Input>) {
            const Input probe{};
            if (auto checked = binding_detail::stable_descriptors(probe); !checked) {
                return std::unexpected(std::move(checked.error()));
            }
            if (auto checked = binding_detail::unique_members<Input>(); !checked) {
                return std::unexpected(std::move(checked.error()));
            }
            auto schema = schema_for<Input>(SchemaUse::input);
            if (!schema) return std::unexpected(std::move(schema.error()));
            for (const auto& member : members_) {
                if (schema->properties.erase(member.field_name) != 1) {
                    return std::unexpected(binding_detail::invalid("bound member is missing from the input schema"));
                }
                const auto removed = std::erase(schema->required, member.field_name);
                if (removed > 1) return std::unexpected(binding_detail::invalid("duplicate required member in the input schema"));
            }
            if (!schema->properties.empty()) {
                if (method == http::HttpMethod::GET || method == http::HttpMethod::HEAD) {
                    return std::unexpected(binding_detail::invalid("GET/HEAD do not support remaining JSON body fields"));
                }
                plan.body_required = !schema->required.empty();
                plan.body_schema = binding_detail::strict_body(std::move(*schema));
            }
        }

        auto runtime_members = members_;
        for (auto& member : runtime_members) member.describe = {};
        plan.decode = [members = std::move(runtime_members), has_body = plan.body_schema.has_value(),
                       required = plan.body_required](http::HttpRequest& request) -> ApiResult<Input> {
            Input input{};
            if constexpr (reflect::StaticReflectable<Input>) {
                if (auto checked = binding_detail::stable_descriptors(input); !checked) {
                    return std::unexpected(std::move(checked.error()));
                }
            }
            if (!has_body && !request.body_str().empty()) {
                return std::unexpected(binding_detail::bad_request("this endpoint does not accept a request body"));
            }
            for (const auto& member : members) {
                std::optional<std::string_view> text;
                if (member.source == ParameterSource::path) {
                    const auto& parameters = request.route_params();
                    const auto found = parameters.find(member.name);
                    if (found != parameters.end()) text = found->second;
                } else if (member.source == ParameterSource::query) {
                    const auto& parameters = request.header().args();
                    const auto found = parameters.find(member.name);
                    if (found != parameters.end()) text = found->second;
                } else return std::unexpected(binding_detail::invalid("unknown parameter source"));
                if (auto decoded = member.decode(input, text); !decoded) return std::unexpected(std::move(decoded.error()));
            }
            if constexpr (reflect::StaticReflectable<Input>) {
                if (has_body) {
                    const bool absent = request.body_str().empty();
                    if (absent && required) return std::unexpected(binding_detail::bad_request("required JSON body is missing"));
                    if (!absent) {
                        const auto* type = request.header().header_pairs().get_value_ptr("Content-Type");
                        if (!type || !binding_detail::json_media_type(*type)) {
                            return std::unexpected(ApiError{ApiErrorCode::kUnsupportedMediaType,
                                "request body requires Content-Type application/json", 415});
                        }
                    }
                    const json::ParseOptions options{.unknown_fields = json::UnknownFieldPolicy::reject};
                    auto body = json::parse(absent ? std::string_view("{}") : std::string_view(request.body_str()), options);
                    if (!body) return std::unexpected(binding_detail::bad_request(std::move(body.error())));
                    const auto selected = [&members](const auto& descriptor) {
                        return std::none_of(members.begin(), members.end(), [&descriptor](const auto& member) {
                            return member.field_name == descriptor.name;
                        });
                    };
                    if (auto decoded = json::decode_fields_into(*body, input, selected, options); !decoded) {
                        return std::unexpected(binding_detail::bad_request(std::move(decoded.error())));
                    }
                    ApiResult<void> defaults;
                    reflect::for_each_field(input, [&](const auto& descriptor, const auto& object) {
                        using Value = std::remove_cvref_t<decltype(descriptor.get(object))>;
                        if constexpr (reflect::is_optional_v<Value>) {
                            if (defaults && selected(descriptor) && !body->contains(descriptor.name)) {
                                defaults = binding_detail::validate_member(descriptor, descriptor.get(object), true);
                            }
                        }
                    });
                    if (!defaults) return std::unexpected(std::move(defaults.error()));
                }
                if (auto checked = binding_detail::stable_descriptors(input); !checked) {
                    return std::unexpected(std::move(checked.error()));
                }
            }
            return input;
        };
        return plan;
    }
}

} // namespace galay::api

#endif
