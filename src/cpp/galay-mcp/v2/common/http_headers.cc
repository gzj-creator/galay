#include "http_headers.h"

#include "../../../galay-utils/encoding/base64.hpp"

#include <cctype>
#include <charconv>
#include <set>

namespace galay::mcp::v2 {

namespace {

bool isTokenChar(unsigned char ch) noexcept
{
    return std::isalnum(ch) != 0 || ch == '!' || ch == '#' || ch == '$' ||
           ch == '%' || ch == '&' || ch == '\'' || ch == '*' || ch == '+' ||
           ch == '-' || ch == '.' || ch == '^' || ch == '_' || ch == '`' ||
           ch == '|' || ch == '~';
}

bool isToken(std::string_view value) noexcept
{
    if (value.empty()) return false;
    for (const unsigned char ch : value) {
        if (!isTokenChar(ch)) return false;
    }
    return true;
}

std::expected<void, McpError> scanSchema(const JsonElement& element,
                                         std::vector<std::string> path,
                                         bool allowAnnotation,
                                         std::set<std::string>& names,
                                         std::vector<HeaderAnnotation>& annotations)
{
    JsonObject object;
    if (JsonHelper::getObject(element, object)) {
        JsonElement annotationElement;
        if (JsonHelper::getElement(object, "x-mcp-header", annotationElement)) {
            if (!allowAnnotation) {
                return std::unexpected(McpError::invalidParams(
                    "x-mcp-header is not statically reachable"));
            }
            std::string name;
            std::string type;
            if (!JsonHelper::getStringValue(annotationElement, name) ||
                !isToken(name) || !JsonHelper::getString(object, "type", type) ||
                (type != "string" && type != "integer" && type != "boolean")) {
                return std::unexpected(McpError::invalidParams(
                    "invalid x-mcp-header annotation"));
            }
            std::string folded = name;
            for (char& ch : folded) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (!names.insert(folded).second) {
                return std::unexpected(McpError::invalidParams(
                    "duplicate x-mcp-header annotation"));
            }
            annotations.push_back(HeaderAnnotation{std::move(name), path, std::move(type)});
        }

        JsonElement propertiesElement;
        JsonObject properties;
        if (JsonHelper::getElement(object, "properties", propertiesElement) &&
            JsonHelper::getObject(propertiesElement, properties)) {
            for (auto field : properties) {
                auto nextPath = path;
                nextPath.emplace_back(field.key);
                auto nested = scanSchema(field.value, std::move(nextPath), true,
                                         names, annotations);
                if (!nested) return std::unexpected(nested.error());
            }
        }

        for (auto field : object) {
            if (std::string_view(field.key) == "properties" ||
                std::string_view(field.key) == "x-mcp-header") {
                continue;
            }
            JsonObject nestedObject;
            JsonArray nestedArray;
            if (JsonHelper::getObject(field.value, nestedObject) ||
                JsonHelper::getArray(field.value, nestedArray)) {
                auto nested = scanSchema(field.value, path, false, names, annotations);
                if (!nested) return std::unexpected(nested.error());
            }
        }
        return {};
    }

    JsonArray array;
    if (JsonHelper::getArray(element, array)) {
        for (auto item : array) {
            auto nested = scanSchema(item, path, false, names, annotations);
            if (!nested) return std::unexpected(nested.error());
        }
    }
    return {};
}

std::expected<std::optional<std::string>, McpError> primitiveValue(
    const JsonElement& element, std::string_view type)
{
    if (element.is_null()) return std::optional<std::string>{};
    if (type == "string") {
        std::string value;
        if (!JsonHelper::getStringValue(element, value)) {
            return std::unexpected(McpError::invalidParams("header parameter type mismatch"));
        }
        return value;
    }
    if (type == "boolean") {
        auto value = element.get_bool();
        if (value.error()) {
            return std::unexpected(McpError::invalidParams("header parameter type mismatch"));
        }
        return value.value() ? std::optional<std::string>("true")
                             : std::optional<std::string>("false");
    }
    auto signedValue = element.get_int64();
    if (!signedValue.error()) {
        constexpr int64_t maxSafe = (int64_t{1} << 53) - 1;
        constexpr int64_t minSafe = -maxSafe;
        if (signedValue.value() < minSafe || signedValue.value() > maxSafe) {
            return std::unexpected(McpError::invalidParams(
                "integer x-mcp-header value exceeds safe range"));
        }
        return std::to_string(signedValue.value());
    }
    auto unsignedValue = element.get_uint64();
    if (!unsignedValue.error() && unsignedValue.value() <= (uint64_t{1} << 53) - 1) {
        return std::to_string(unsignedValue.value());
    }
    return std::unexpected(McpError::invalidParams("header parameter type mismatch"));
}

} // namespace

bool safeHeaderValue(std::string_view value) noexcept
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

std::string encodeHeaderValue(std::string_view value)
{
    if (safeHeaderValue(value)) return std::string(value);
    return "=?base64?" + galay::utils::Base64Util::Base64EncodeView(value) + "?=";
}

std::expected<std::string, McpError> decodeHeaderValue(std::string_view value)
{
    constexpr std::string_view prefix = "=?base64?";
    constexpr std::string_view suffix = "?=";
    if (value.starts_with(prefix) || value.ends_with(suffix)) {
        if (value.size() <= prefix.size() + suffix.size() ||
            !value.starts_with(prefix) || !value.ends_with(suffix)) {
            return std::unexpected(McpError::protocolError("invalid encoded header value"));
        }
        const auto encoded = value.substr(prefix.size(), value.size() - prefix.size() - suffix.size());
        if (!galay::utils::Base64Util::Base64CanDecodeView(encoded)) {
            return std::unexpected(McpError::protocolError("invalid encoded header value"));
        }
        return galay::utils::Base64Util::Base64DecodeView(encoded);
    }
    if (!safeHeaderValue(value)) {
        return std::unexpected(McpError::protocolError("invalid header value"));
    }
    return std::string(value);
}

std::expected<std::vector<HeaderAnnotation>, McpError>
toolHeaderAnnotations(const Tool& tool)
{
    auto document = JsonDocument::parse(tool.inputSchema);
    if (!document) return std::unexpected(document.error());
    JsonObject object;
    if (!JsonHelper::getObject(document->root(), object)) {
        return std::unexpected(McpError::invalidParams("tool inputSchema must be an object"));
    }
    std::set<std::string> names;
    std::vector<HeaderAnnotation> annotations;
    auto result = scanSchema(document->root(), {}, false, names, annotations);
    if (!result) return std::unexpected(result.error());
    return annotations;
}

std::expected<std::optional<std::string>, McpError>
argumentHeaderValue(const JsonElement& arguments, const HeaderAnnotation& annotation)
{
    JsonElement current = arguments;
    for (const auto& key : annotation.path) {
        JsonObject object;
        if (!JsonHelper::getObject(current, object) ||
            !JsonHelper::getElement(object, key.c_str(), current)) {
            return std::optional<std::string>{};
        }
    }
    return primitiveValue(current, annotation.type);
}

} // namespace galay::mcp::v2
