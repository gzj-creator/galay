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

struct AnnotationFields {
    std::optional<std::string> name;
    std::optional<std::string> type;
};
constexpr auto reflect_fields(std::type_identity<AnnotationFields>) {
    return std::make_tuple(
        json::make_field("x-mcp-header", &AnnotationFields::name, json::FieldPolicy{.reject_null = true}),
        json::make_field("type", &AnnotationFields::type));
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
    auto root = json::deserialize<json::Object>(tool.inputSchema);
    if (!root) return std::unexpected(McpError::invalid_params(root.error()));
    std::set<std::string> names;
    std::vector<HeaderAnnotation> annotations;
    auto visited = json::visit_objects(tool.inputSchema, [&](const json::Json& object,
        const std::vector<std::string>& schema_path) -> json::result<void> {
        auto fields = json::decode<AnnotationFields>(object);
        if (!fields) return std::unexpected(fields.error());
        if (!fields->name) return {};
        if (!fields->type || !is_token(*fields->name) ||
            (*fields->type != "string" && *fields->type != "integer" && *fields->type != "boolean"))
            return std::unexpected(std::string("invalid x-mcp-header annotation"));
        std::vector<std::string> argument_path;
        if (schema_path.empty() || schema_path.size() % 2 != 0)
            return std::unexpected(std::string("x-mcp-header is not statically reachable"));
        for (std::size_t index = 0; index < schema_path.size(); index += 2) {
            if (schema_path[index] != "properties")
                return std::unexpected(std::string("x-mcp-header is not statically reachable"));
            argument_path.push_back(schema_path[index + 1]);
        }
        std::string folded = *fields->name;
        for (char& ch : folded) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!names.insert(folded).second)
            return std::unexpected(std::string("duplicate x-mcp-header annotation"));
        annotations.push_back({std::move(*fields->name), std::move(argument_path), std::move(*fields->type)});
        return {};
    });
    if (!visited) return std::unexpected(McpError::invalid_params(visited.error()));
    return annotations;
}

std::expected<std::optional<std::string>, McpError>
argument_header_value(const json::Json& arguments, const HeaderAnnotation& annotation) {
    if (annotation.type == "string") {
        auto value = json::decode_path<std::optional<std::string>>(arguments, annotation.path);
        if (!value) return std::unexpected(McpError::invalid_params(value.error()));
        return std::move(*value);
    }
    if (annotation.type == "boolean") {
        auto value = json::decode_path<std::optional<bool>>(arguments, annotation.path);
        if (!value) return std::unexpected(McpError::invalid_params(value.error()));
        if (!*value) return std::optional<std::string>{};
        return std::optional<std::string>{**value ? "true" : "false"};
    }
    auto value = json::decode_path<std::optional<int64_t>>(arguments, annotation.path);
    if (!value) return std::unexpected(McpError::invalid_params(value.error()));
    if (!*value) return std::optional<std::string>{};
    constexpr int64_t max_safe = (int64_t{1} << 53) - 1;
    if (**value < -max_safe || **value > max_safe)
        return std::unexpected(McpError::invalid_params("integer x-mcp-header value exceeds safe range"));
    return std::optional<std::string>{std::to_string(**value)};
}

} // namespace galay::mcp::v2
