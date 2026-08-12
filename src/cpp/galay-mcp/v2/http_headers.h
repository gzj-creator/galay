/**
 * @file http_headers.h
 * @brief MCP 2026-07-28 mirrored HTTP header helpers.
 */

#ifndef GALAY_MCP_V2_HTTP_HEADERS_H
#define GALAY_MCP_V2_HTTP_HEADERS_H

#include "protocol.h"

#include <optional>

namespace galay::mcp::v2 {

struct HeaderAnnotation {
    std::string name;
    std::vector<std::string> path;
    std::string type;
};

std::expected<std::vector<HeaderAnnotation>, McpError>
toolHeaderAnnotations(const Tool& tool);

std::expected<std::optional<std::string>, McpError>
argumentHeaderValue(const JsonElement& arguments, const HeaderAnnotation& annotation);

bool safeHeaderValue(std::string_view value) noexcept;
std::string encodeHeaderValue(std::string_view value);
std::expected<std::string, McpError> decodeHeaderValue(std::string_view value);

} // namespace galay::mcp::v2

#endif // GALAY_MCP_V2_HTTP_HEADERS_H
