#include "http_headers.h"

#include "../../../galay-utils/encoding/base64.hpp"

#include <cctype>
#include <charconv>
#include <set>

namespace galay::mcp::v2 {

namespace {

bool is_token_char(unsigned char ch) noexcept
{
    return std::isalnum(ch) != 0 || ch == '!' || ch == '#' || ch == '$' ||
           ch == '%' || ch == '&' || ch == '\'' || ch == '*' || ch == '+' ||
           ch == '-' || ch == '.' || ch == '^' || ch == '_' || ch == '`' ||
           ch == '|' || ch == '~';
}

bool is_token(std::string_view value) noexcept
{
    if (value.empty()) return false;
    for (const unsigned char ch : value) {
        if (!is_token_char(ch)) return false;
    }
    return true;
}

std::expected<void, McpError> scan_schema(const json::Json& element,
                                         std::vector<std::string> path,
                                         bool allowAnnotation,
                                         std::set<std::string>& names,
                                         std::vector<HeaderAnnotation>& annotations)
{
    if (element.is_object()) {
        const json::Json& object = element;
        const json::Json annotationElement = object.at("x-mcp-header");
        if (annotationElement.valid()) {
            if (!allowAnnotation) {
                return std::unexpected(McpError::invalid_params(
                    "x-mcp-header is not statically reachable"));
            }
            const auto name = annotationElement.as_string();
            const auto type = object.at("type").as_string();
            if (!name || !is_token(*name) || !type ||
                (*type != "string" && *type != "integer" && *type != "boolean")) {
                return std::unexpected(McpError::invalid_params(
                    "invalid x-mcp-header annotation"));
            }
            std::string folded(*name);
            for (char& ch : folded) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (!names.insert(folded).second) {
                return std::unexpected(McpError::invalid_params(
                    "duplicate x-mcp-header annotation"));
            }
            annotations.push_back(HeaderAnnotation{std::string(*name), path, std::string(*type)});
        }

        const json::Json properties = object.at("properties");
        if (properties.is_object()) {
            std::optional<McpError> failure;
            properties.for_each_member([&](std::string_view fieldKey, const json::Json& value) -> json::result<void> {
                auto nextPath = path;
                nextPath.emplace_back(fieldKey);
                auto nested = scan_schema(value, std::move(nextPath), true,
                                         names, annotations);
                if (!nested) {
                    failure = nested.error();
                    return std::unexpected(std::string("stop"));
                }
                return {};
            });
            if (failure) return std::unexpected(*failure);
        }

        std::optional<McpError> failure;
        object.for_each_member([&](std::string_view fieldKey, const json::Json& value) -> json::result<void> {
            if (fieldKey == "properties" || fieldKey == "x-mcp-header") {
                return {};
            }
            if (value.is_object() || value.is_array()) {
                auto nested = scan_schema(value, path, false, names, annotations);
                if (!nested) {
                    failure = nested.error();
                    return std::unexpected(std::string("stop"));
                }
            }
            return {};
        });
        if (failure) return std::unexpected(*failure);
        return {};
    }

    if (element.is_array()) {
        for (size_t i = 0; i < element.size(); ++i) {
            const json::Json item = element.at(i);
            auto nested = scan_schema(item, path, false, names, annotations);
            if (!nested) return std::unexpected(nested.error());
        }
    }
    return {};
}

std::expected<std::optional<std::string>, McpError> primitive_value(
    const json::Json& element, std::string_view type)
{
    if (element.is_null()) return std::optional<std::string>{};
    if (type == "string") {
        auto value = element.as_string();
        if (!value) {
            return std::unexpected(McpError::invalid_params("header parameter type mismatch"));
        }
        return std::string(*value);
    }
    if (type == "boolean") {
        auto value = element.as_bool();
        if (!value.has_value()) {
            return std::unexpected(McpError::invalid_params("header parameter type mismatch"));
        }
        return value.value() ? std::optional<std::string>("true")
                             : std::optional<std::string>("false");
    }
    auto signedValue = element.as_int64();
    if (signedValue.has_value()) {
        constexpr int64_t maxSafe = (int64_t{1} << 53) - 1;
        constexpr int64_t minSafe = -maxSafe;
        if (signedValue.value() < minSafe || signedValue.value() > maxSafe) {
            return std::unexpected(McpError::invalid_params(
                "integer x-mcp-header value exceeds safe range"));
        }
        return std::to_string(signedValue.value());
    }
    auto unsignedValue = element.as_uint64();
    if (unsignedValue.has_value() && unsignedValue.value() <= (uint64_t{1} << 53) - 1) {
        return std::to_string(unsignedValue.value());
    }
    return std::unexpected(McpError::invalid_params("header parameter type mismatch"));
}

} // namespace

bool safe_header_value(std::string_view value) noexcept
{
    if (value.empty() || value.front() == ' ' || value.back() == ' ' ||
        value.front() == '\t' || value.back() == '\t') {
        return false;
    }
    for (const unsigned char ch : value) {
        if (ch < 0x20 || ch > 0x7e) return false;
    }
    return true;
}

std::string encode_header_value(std::string_view value)
{
    if (safe_header_value(value)) return std::string(value);
    return "=?base64?" + galay::utils::Base64Util::base64_encode_view(value) + "?=";
}

std::expected<std::string, McpError> decode_header_value(std::string_view value)
{
    constexpr std::string_view prefix = "=?base64?";
    constexpr std::string_view suffix = "?=";
    if (value.starts_with(prefix) || value.ends_with(suffix)) {
        if (value.size() <= prefix.size() + suffix.size() ||
            !value.starts_with(prefix) || !value.ends_with(suffix)) {
            return std::unexpected(McpError::protocol_error("invalid encoded header value"));
        }
        const auto encoded = value.substr(prefix.size(), value.size() - prefix.size() - suffix.size());
        if (!galay::utils::Base64Util::base64_can_decode_view(encoded)) {
            return std::unexpected(McpError::protocol_error("invalid encoded header value"));
        }
        return galay::utils::Base64Util::base64_decode_view(encoded);
    }
    if (!safe_header_value(value)) {
        return std::unexpected(McpError::protocol_error("invalid header value"));
    }
    return std::string(value);
}

std::expected<std::vector<HeaderAnnotation>, McpError>
tool_header_annotations(const Tool& tool)
{
    auto document = JsonDocument::parse(tool.inputSchema);
    if (!document) return std::unexpected(document.error());
    if (!document->root().is_object()) {
        return std::unexpected(McpError::invalid_params("tool inputSchema must be an object"));
    }
    std::set<std::string> names;
    std::vector<HeaderAnnotation> annotations;
    auto result = scan_schema(document->root(), {}, false, names, annotations);
    if (!result) return std::unexpected(result.error());
    return annotations;
}

std::expected<std::optional<std::string>, McpError>
argument_header_value(const json::Json& arguments, const HeaderAnnotation& annotation)
{
    json::Json current = arguments;
    for (const auto& key : annotation.path) {
        if (!current.is_object()) {
            return std::optional<std::string>{};
        }
        current = current.at(key);
        if (!current.valid()) {
            return std::optional<std::string>{};
        }
    }
    return primitive_value(current, annotation.type);
}

} // namespace galay::mcp::v2
