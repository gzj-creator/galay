/**
 * @file protocol_utils.h
 * @brief MCP协议消息构建工具函数
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 提供用于构建MCP协议JSON-RPC消息的内联工具函数，
 *          包括初始化结果、成功/错误响应、请求体构建和列表结果构建。
 */

#ifndef GALAY_MCP_COMMON_MCPPROTOCOLUTILS_H
#define GALAY_MCP_COMMON_MCPPROTOCOLUTILS_H

#include "mcp_base.h"
#include "mcp_json.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace galay::mcp {
namespace protocol {

/**
 * @brief 构建初始化响应结果
 * @param serverName 服务器名称
 * @param serverVersion 服务器版本
 * @param hasTools 是否支持工具功能
 * @param hasResources 是否支持资源功能
 * @param hasPrompts 是否支持提示功能
 * @return 初始化结果的JSON字符串
 */
inline JsonString buildInitializeResult(const std::string& serverName,
                                        const std::string& serverVersion,
                                        bool hasTools,
                                        bool hasResources,
                                        bool hasPrompts) {
    InitializeResult result;
    result.protocolVersion = MCP_VERSION;
    result.serverInfo.name = serverName;
    result.serverInfo.version = serverVersion;
    result.serverInfo.capabilities = "{}";

    result.capabilities.tools = hasTools;
    result.capabilities.resources = hasResources;
    result.capabilities.prompts = hasPrompts;
    result.capabilities.logging = false;

    return result.toJson();
}

/**
 * @brief 构建成功结果响应
 * @param id 请求标识符
 * @param result 结果JSON字符串
 * @return 包含成功结果的JsonRpcResponse
 */
inline JsonRpcResponse makeResultResponse(int64_t id, const JsonString& result) {
    JsonRpcResponse response;
    response.id = id;
    response.result = result;
    return response;
}

/**
 * @brief Build a compact JSON-RPC request body for HTTP transport.
 * @param id Request identifier written into the JSON-RPC envelope.
 * @param method JSON-RPC method name. The caller must pass a valid UTF-8 name.
 * @param params Optional serialized JSON value. When provided as an empty string,
 *        this helper emits `params: {}` so callers can request an empty object
 *        without creating another temporary buffer.
 * @return Serialized JSON request body ready to send as an HTTP payload.
 */
inline JsonString makeJsonRpcRequestBody(int64_t id,
                                         std::string_view method,
                                         std::optional<std::string_view> params = std::nullopt) {
    const std::string id_string = std::to_string(id);
    const size_t params_size = params.has_value() ? params->size() : 0;

    JsonString body;
    body.reserve(48 + id_string.size() + method.size() + params_size);
    body += "{\"jsonrpc\":\"2.0\",\"id\":";
    body += id_string;
    body += ",\"method\":\"";
    body.append(method.data(), method.size());
    body.push_back('"');

    if (params.has_value()) {
        body += ",\"params\":";
        if (params->empty()) {
            body += "{}";
        } else {
            body.append(params->data(), params->size());
        }
    }

    body.push_back('}');
    return body;
}

/**
 * @brief 构建错误响应
 * @param id 请求标识符
 * @param code 错误码
 * @param message 错误消息
 * @param details 错误详情（可选）
 * @return 包含错误信息的JsonRpcResponse
 */
inline JsonRpcResponse makeErrorResponse(int64_t id,
                                         int code,
                                         const std::string& message,
                                         const std::string& details = "") {
    JsonRpcError error;
    error.code = code;
    error.message = message;
    if (!details.empty()) {
        JsonWriter writer;
        writer.string(details);
        error.data = writer.takeString();
    }

    JsonRpcResponse response;
    response.id = id;
    response.error = error.toJson();
    return response;
}

/**
 * @brief 从映射表构建列表结果
 * @tparam MapType 映射表类型
 * @tparam Extractor 值提取函数类型
 * @param map 工具/资源/提示的映射表
 * @param key 结果JSON中的列表字段名
 * @param extractor 从映射值提取JSON的函数
 * @return 列表结果的JSON字符串
 */
template <typename MapType, typename Extractor>
JsonString buildListResultFromMap(const MapType& map, const char* key, Extractor extractor) {
    JsonWriter writer;
    writer.startObject();
    writer.key(key);
    writer.startArray();
    for (const auto& [name, info] : map) {
        writer.raw(extractor(info).toJson());
    }
    writer.endArray();
    writer.endObject();
    return writer.takeString();
}

} // namespace protocol
} // namespace galay::mcp

#endif // GALAY_MCP_COMMON_MCPPROTOCOLUTILS_H
