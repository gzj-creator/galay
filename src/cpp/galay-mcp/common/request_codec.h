/**
 * @file request_codec.h
 * @brief JSON-RPC协议解码与校验
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 提供JSON-RPC 2.0格式的请求和响应消息解析功能，
 *          JSON处理由serde完成，本文件只定义协议结果与校验入口。
 */

#ifndef GALAY_MCP_COMMON_REQUEST_CODEC_H
#define GALAY_MCP_COMMON_REQUEST_CODEC_H

#include "mcp_base.h"
#include "mcp_error.h"
#include <expected>
#include <string>
#include <string_view>

namespace galay::mcp {

/**
 * @brief JSON-RPC请求视图
 * @details 参数值由serde管理存储
 */
struct JsonRpcRequestView {
    std::optional<int64_t> id; ///< 请求标识符（通知消息无此字段）
    std::string method; ///< JSON-RPC方法名
    json::Json params; ///< 请求参数元素
    bool hasParams = false; ///< 是否包含参数
};

/**
 * @brief 已解析的JSON-RPC请求
 * @details 持有解码后的请求字段
 */
struct ParsedJsonRpcRequest {
    ParsedJsonRpcRequest() = default; ///< 默认构造
    ParsedJsonRpcRequest(ParsedJsonRpcRequest&&) noexcept = default; ///< 移动构造，保持DOM地址稳定
    /**
     * @brief 移动赋值，保持DOM地址稳定
     * @return 当前对象引用
     */
    ParsedJsonRpcRequest& operator=(ParsedJsonRpcRequest&&) noexcept = default;

    JsonRpcRequestView request; ///< 解析出的请求视图

private:
    ParsedJsonRpcRequest(const ParsedJsonRpcRequest&) = delete; ///< 禁止隐式复制DOM与视图
    /**
     * @brief 禁止隐式复制DOM与视图
     * @return 该操作已禁用，不可调用
     */
    ParsedJsonRpcRequest& operator=(const ParsedJsonRpcRequest&) = delete;
};

/**
 * @brief JSON-RPC响应视图
 * @details 结果和错误值由serde管理存储
 */
struct JsonRpcResponseView {
    int64_t id = 0; ///< 响应对应的请求标识符
    json::Json result; ///< 响应结果元素
    json::Json error; ///< 响应错误元素
    bool hasResult = false; ///< 是否包含结果
    bool hasError = false; ///< 是否包含错误
};

/**
 * @brief 已解析的JSON-RPC响应
 * @details 持有解码后的响应字段
 */
struct ParsedJsonRpcResponse {
    ParsedJsonRpcResponse() = default; ///< 默认构造
    ParsedJsonRpcResponse(ParsedJsonRpcResponse&&) noexcept = default; ///< 移动构造，保持DOM地址稳定
    /**
     * @brief 移动赋值，保持DOM地址稳定
     * @return 当前对象引用
     */
    ParsedJsonRpcResponse& operator=(ParsedJsonRpcResponse&&) noexcept = default;

    JsonRpcResponseView response; ///< 解析出的响应视图

private:
    ParsedJsonRpcResponse(const ParsedJsonRpcResponse&) = delete; ///< 禁止隐式复制DOM与视图
    /**
     * @brief 禁止隐式复制DOM与视图
     * @return 该操作已禁用，不可调用
     */
    ParsedJsonRpcResponse& operator=(const ParsedJsonRpcResponse&) = delete;
};

/**
 * @brief 从原始JSON文本解析JSON-RPC请求
 * @param body 原始JSON请求文本
 * @return 成功返回ParsedJsonRpcRequest，失败返回McpError
 */
std::expected<ParsedJsonRpcRequest, McpError> parse_json_rpc_request(std::string_view body);

/**
 * @brief 从原始JSON文本解析JSON-RPC响应
 * @param body 原始JSON响应文本
 * @return 成功返回ParsedJsonRpcResponse，失败返回McpError
 */
std::expected<ParsedJsonRpcResponse, McpError> parse_json_rpc_response(std::string_view body);

} // namespace galay::mcp

#endif // GALAY_MCP_COMMON_REQUEST_CODEC_H
