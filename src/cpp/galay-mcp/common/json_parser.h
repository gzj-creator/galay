/**
 * @file json_parser.h
 * @brief JSON-RPC消息解析器
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 提供JSON-RPC 2.0格式的请求和响应消息解析功能，
 *          将原始JSON文本解析为结构化的视图对象。
 */

#ifndef GALAY_MCP_COMMON_MCPJSONPARSER_H
#define GALAY_MCP_COMMON_MCPJSONPARSER_H

#include "mcp_base.h"
#include "mcp_error.h"
#include "mcp_json.h"
#include <expected>
#include <string>
#include <string_view>

namespace galay {
namespace mcp {

/**
 * @brief JSON-RPC请求视图
 * @details 零拷贝方式引用已解析JSON文档中的请求字段
 */
struct JsonRpcRequestView {
    std::optional<int64_t> id; ///< 请求标识符（通知消息无此字段）
    std::string method; ///< JSON-RPC方法名
    JsonElement params; ///< 请求参数元素
    bool hasParams = false; ///< 是否包含参数
};

/**
 * @brief 已解析的JSON-RPC请求
 * @details 持有JSON文档及其解析出的请求视图
 */
struct ParsedJsonRpcRequest {
    ParsedJsonRpcRequest() = default; ///< 默认构造
    ParsedJsonRpcRequest(ParsedJsonRpcRequest&&) noexcept = default; ///< 移动构造，保持DOM地址稳定
    ParsedJsonRpcRequest& operator=(ParsedJsonRpcRequest&&) noexcept = default; ///< 移动赋值，保持DOM地址稳定

    JsonDocument document; ///< JSON文档（持有底层数据）
    JsonRpcRequestView request; ///< 解析出的请求视图

private:
    ParsedJsonRpcRequest(const ParsedJsonRpcRequest&) = delete; ///< 禁止隐式复制DOM与视图
    ParsedJsonRpcRequest& operator=(const ParsedJsonRpcRequest&) = delete; ///< 禁止隐式复制DOM与视图
};

/**
 * @brief JSON-RPC响应视图
 * @details 零拷贝方式引用已解析JSON文档中的响应字段
 */
struct JsonRpcResponseView {
    int64_t id = 0; ///< 响应对应的请求标识符
    JsonElement result; ///< 响应结果元素
    JsonElement error; ///< 响应错误元素
    bool hasResult = false; ///< 是否包含结果
    bool hasError = false; ///< 是否包含错误
};

/**
 * @brief 已解析的JSON-RPC响应
 * @details 持有JSON文档及其解析出的响应视图
 */
struct ParsedJsonRpcResponse {
    ParsedJsonRpcResponse() = default; ///< 默认构造
    ParsedJsonRpcResponse(ParsedJsonRpcResponse&&) noexcept = default; ///< 移动构造，保持DOM地址稳定
    ParsedJsonRpcResponse& operator=(ParsedJsonRpcResponse&&) noexcept = default; ///< 移动赋值，保持DOM地址稳定

    JsonDocument document; ///< JSON文档（持有底层数据）
    JsonRpcResponseView response; ///< 解析出的响应视图

private:
    ParsedJsonRpcResponse(const ParsedJsonRpcResponse&) = delete; ///< 禁止隐式复制DOM与视图
    ParsedJsonRpcResponse& operator=(const ParsedJsonRpcResponse&) = delete; ///< 禁止隐式复制DOM与视图
};

/**
 * @brief 从原始JSON文本解析JSON-RPC请求
 * @param body 原始JSON请求文本
 * @return 成功返回ParsedJsonRpcRequest，失败返回McpError
 */
std::expected<ParsedJsonRpcRequest, McpError> parseJsonRpcRequest(std::string_view body);

/**
 * @brief 从原始JSON文本解析JSON-RPC响应
 * @param body 原始JSON响应文本
 * @return 成功返回ParsedJsonRpcResponse，失败返回McpError
 */
std::expected<ParsedJsonRpcResponse, McpError> parseJsonRpcResponse(std::string_view body);

} // namespace mcp
} // namespace galay

#endif // GALAY_MCP_COMMON_MCPJSONPARSER_H
