#include "mcp_json.h"

namespace galay::mcp {

std::expected<JsonDocument, McpError> JsonDocument::parse(std::string_view json) {
    auto parsed = json::parse(json);
    if (!parsed) {
        return std::unexpected(McpError::parse_error(std::move(parsed.error())));
    }
    JsonDocument doc;
    doc.m_root = std::move(parsed.value());
    return doc;
}

} // namespace galay::mcp
