/**
 * @file mcp_base.h
 * @brief MCP协议基础类型定义
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 定义MCP（Model Context Protocol）协议的核心数据结构，
 *          包括消息类型、方法名称、内容类型、工具/资源/提示定义、
 *          JSON-RPC请求/响应/通知/错误结构等。
 */

#ifndef GALAY_MCP_COMMON_MCPBASE_H
#define GALAY_MCP_COMMON_MCPBASE_H

#include "mcp_json.h"
#include "../../galay-kernel/core/task.h"
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace galay {
namespace mcp {


constexpr const char* MCP_VERSION = "2024-11-05"; ///< MCP协议版本号
constexpr const char* JSONRPC_VERSION = "2.0"; ///< JSON-RPC协议版本号

/**
 * @brief MCP消息类型枚举
 */
enum class MessageType {
    Request, ///< 请求消息
    Response, ///< 响应消息
    Notification, ///< 通知消息
    Error ///< 错误消息
};

/**
 * @brief MCP协议方法名称常量
 */
namespace Methods {
    constexpr const char* INITIALIZE = "initialize"; ///< 初始化方法
    constexpr const char* INITIALIZED = "notifications/initialized"; ///< 初始化完成通知
    constexpr const char* PING = "ping"; ///< 心跳检测方法
    constexpr const char* TOOLS_LIST = "tools/list"; ///< 获取工具列表
    constexpr const char* TOOLS_CALL = "tools/call"; ///< 调用工具
    constexpr const char* RESOURCES_LIST = "resources/list"; ///< 获取资源列表
    constexpr const char* RESOURCES_READ = "resources/read"; ///< 读取资源
    constexpr const char* PROMPTS_LIST = "prompts/list"; ///< 获取提示列表
    constexpr const char* PROMPTS_GET = "prompts/get"; ///< 获取提示
}

/**
 * @brief 内容类型枚举
 */
enum class ContentType {
    Text, ///< 文本内容
    Image, ///< 图片内容
    Resource ///< 资源内容
};

/**
 * @brief 内容项结构
 * @details 表示MCP协议中的内容项，可以是文本、图片或资源引用
 */
struct Content {
    std::string text; ///< 文本内容（Text类型使用）
    std::string data; ///< 图片数据（Image类型使用，base64编码）
    std::string mimeType; ///< MIME类型（Image类型使用）
    std::string uri; ///< 资源URI（Resource类型使用）
    ContentType type{ContentType::Text}; ///< 内容类型

    /**
     * @brief 序列化为JSON字符串
     * @return JSON格式字符串
     */
    JsonString toJson() const;
    /**
     * @brief 从JSON元素反序列化
     * @param element JSON元素
     * @return 成功返回Content，失败返回McpError
     */
    static std::expected<Content, McpError> fromJson(const JsonElement& element);
};

/**
 * @brief 工具定义结构
 * @details 描述MCP服务器提供的工具，包括名称、描述和输入参数Schema
 */
struct Tool {
    std::string name; ///< 工具名称
    std::string description; ///< 工具描述
    JsonString inputSchema; ///< 输入参数的JSON Schema

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<Tool, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief 资源定义结构
 * @details 描述MCP服务器提供的资源，包括URI、名称、描述和MIME类型
 */
struct Resource {
    std::string uri; ///< 资源URI标识
    std::string name; ///< 资源名称
    std::string description; ///< 资源描述
    std::string mimeType; ///< 资源MIME类型

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<Resource, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief 提示参数定义结构
 * @details 描述提示模板中的单个参数
 */
struct PromptArgument {
    std::string name; ///< 参数名称
    std::string description; ///< 参数描述
    bool required{false}; ///< 是否为必填参数

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<PromptArgument, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief 提示定义结构
 * @details 描述MCP服务器提供的提示模板
 */
struct Prompt {
    std::string name; ///< 提示名称
    std::string description; ///< 提示描述
    std::vector<PromptArgument> arguments; ///< 提示参数列表

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<Prompt, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief 客户端信息结构
 * @details 描述MCP客户端的基本信息
 */
struct ClientInfo {
    std::string name; ///< 客户端名称
    std::string version; ///< 客户端版本

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<ClientInfo, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief 服务器信息结构
 * @details 描述MCP服务器的基本信息和能力声明
 */
struct ServerInfo {
    std::string name; ///< 服务器名称
    std::string version; ///< 服务器版本
    JsonString capabilities; ///< 服务器能力JSON

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<ServerInfo, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief 服务器能力结构
 * @details 声明MCP服务器支持的功能特性
 */
struct ServerCapabilities {
    bool tools = false; ///< 是否支持工具功能
    bool resources = false; ///< 是否支持资源功能
    bool prompts = false; ///< 是否支持提示功能
    bool logging = false; ///< 是否支持日志功能

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<ServerCapabilities, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief 初始化请求参数结构
 * @details 客户端发送的initialize请求中携带的参数
 */
struct InitializeParams {
    std::string protocolVersion; ///< 协议版本号
    ClientInfo clientInfo; ///< 客户端信息
    JsonString capabilities; ///< 客户端能力JSON

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<InitializeParams, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief 初始化响应结果结构
 * @details 服务器响应initialize请求时返回的结果
 */
struct InitializeResult {
    std::string protocolVersion; ///< 协议版本号
    ServerInfo serverInfo; ///< 服务器信息
    ServerCapabilities capabilities; ///< 服务器能力

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<InitializeResult, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief 工具调用参数结构
 * @details 调用工具时传递的参数
 */
struct ToolCallParams {
    std::string name; ///< 工具名称
    JsonString arguments; ///< 工具参数JSON

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<ToolCallParams, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief 工具调用结果结构
 * @details 工具执行后返回的结果
 */
struct ToolCallResult {
    std::vector<Content> content; ///< 内容列表
    bool isError = false; ///< 是否为错误结果

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<ToolCallResult, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief JSON-RPC请求结构
 * @details 用于生成JSON-RPC 2.0格式的请求消息
 */
struct JsonRpcRequest {
    std::string jsonrpc = JSONRPC_VERSION; ///< JSON-RPC版本号
    std::optional<int64_t> id; ///< 请求标识符（通知消息无此字段）
    std::string method; ///< 方法名称
    std::optional<JsonString> params; ///< 请求参数

    JsonString toJson() const; ///< 序列化为JSON字符串
};

/**
 * @brief JSON-RPC响应结构
 * @details 表示JSON-RPC 2.0格式的响应消息
 */
struct JsonRpcResponse {
    std::string jsonrpc = JSONRPC_VERSION; ///< JSON-RPC版本号
    int64_t id = 0; ///< 响应对应的请求标识符
    std::optional<JsonString> result; ///< 成功时的结果
    std::optional<JsonString> error; ///< 失败时的错误信息

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<JsonRpcResponse, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief JSON-RPC通知结构
 * @details 表示JSON-RPC 2.0格式的通知消息（无ID，不需要响应）
 */
struct JsonRpcNotification {
    std::string jsonrpc = JSONRPC_VERSION; ///< JSON-RPC版本号
    std::string method; ///< 通知方法名称
    std::optional<JsonString> params; ///< 通知参数

    JsonString toJson() const; ///< 序列化为JSON字符串
};

/**
 * @brief JSON-RPC错误结构
 * @details 表示JSON-RPC 2.0格式的错误信息
 */
struct JsonRpcError {
    std::string message; ///< 错误消息
    std::optional<JsonString> data; ///< 附加错误数据
    int code = 0; ///< 错误码

    JsonString toJson() const; ///< 序列化为JSON字符串
    static std::expected<JsonRpcError, McpError> fromJson(const JsonElement& element); ///< 从JSON元素反序列化
};

/**
 * @brief JSON-RPC标准错误码常量
 */
namespace ErrorCodes {
    constexpr int PARSE_ERROR = -32700; ///< 解析错误
    constexpr int INVALID_REQUEST = -32600; ///< 无效请求
    constexpr int METHOD_NOT_FOUND = -32601; ///< 方法未找到
    constexpr int INVALID_PARAMS = -32602; ///< 无效参数
    constexpr int INTERNAL_ERROR = -32603; ///< 内部错误
    constexpr int SERVER_ERROR_START = -32099; ///< 服务器错误范围起始
    constexpr int SERVER_ERROR_END = -32000; ///< 服务器错误范围结束
}

} // namespace mcp
} // namespace galay

#endif // GALAY_MCP_COMMON_MCPBASE_H
