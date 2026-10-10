#ifndef GALAY_MCP_COMMON_MCPPROTOCOLUTILS_H
#define GALAY_MCP_COMMON_MCPPROTOCOLUTILS_H
#include "mcp_base.h"
#include <map>

namespace galay::mcp::protocol {
inline std::string build_initialize_result(const std::string& server_name,
    const std::string& server_version, bool has_tools, bool has_resources, bool has_prompts) {
    InitializeResult result;
    result.protocolVersion = MCP_VERSION;
    result.serverInfo = {server_name, server_version, "{}"};
    result.capabilities = {has_tools, has_resources, has_prompts, false};
    return result.encode();
}
inline JsonRpcResponse make_result_response(int64_t id, const std::string& result) {
    JsonRpcResponse response;
    response.id = id;
    response.result = result;
    return response;
}
inline std::string make_json_rpc_request_body(int64_t id, std::string_view method,
    std::optional<std::string_view> params = std::nullopt) {
    JsonRpcRequest request;
    request.id = id;
    request.method = method;
    if (params) request.params = params->empty() ? "{}" : std::string(*params);
    return request.encode();
}
inline JsonRpcResponse make_error_response(int64_t id, int code,
    const std::string& message, const std::string& details = "") {
    JsonRpcError error;
    error.code = code;
    error.message = message;
    if (!details.empty()) {
        auto data = json::serialize(details);
        if (!data) return {};
        error.data = std::move(*data);
    }
    JsonRpcResponse response;
    response.id = id;
    response.error = error.encode();
    return response;
}
template <typename MapType, typename Extractor>
std::string build_list_result_from_map(const MapType& map, const char* key, Extractor extractor) {
    using Item = std::remove_cvref_t<decltype(extractor(map.begin()->second))>;
    std::map<std::string, std::vector<Item>> result;
    auto& items = result[key];
    for (const auto& [name, info] : map) items.push_back(extractor(info));
    auto body = json::serialize(result);
    if (!body) return {};
    return std::move(*body);
}
} // namespace galay::mcp::protocol
#endif
