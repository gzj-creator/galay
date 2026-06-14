/**
 * @file mcp_error.h
 * @brief MCP协议错误类型与错误处理
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 定义MCP协议中的错误码枚举、错误类以及各类静态工厂方法，
 *          覆盖连接、协议、JSON-RPC、工具、资源、提示等错误场景。
 */

#ifndef GALAY_MCP_COMMON_MCPERROR_H
#define GALAY_MCP_COMMON_MCPERROR_H

#include <string>
#include <system_error>

namespace galay {
namespace mcp {

/**
 * @brief MCP错误码枚举
 * @details 按错误类别分段定义：连接(1xxx)、协议(2xxx)、JSON-RPC(3xxx)、
 *          工具(4xxx)、资源(5xxx)、提示(6xxx)、初始化(7xxx)、IO(8xxx)
 */
enum class McpErrorCode {
    Success = 0, ///< 成功，无错误

    ConnectionFailed = 1000, ///< 连接失败
    ConnectionClosed = 1001, ///< 连接已关闭
    ConnectionTimeout = 1002, ///< 连接超时

    ProtocolError = 2000, ///< 协议错误
    InvalidMessage = 2001, ///< 无效消息
    InvalidMethod = 2002, ///< 无效方法
    InvalidParams = 2003, ///< 无效参数

    ParseError = 3000, ///< JSON解析错误
    InvalidRequest = 3001, ///< 无效请求
    MethodNotFound = 3002, ///< 方法未找到
    InternalError = 3003, ///< 内部错误

    ToolNotFound = 4000, ///< 工具未找到
    ToolExecutionFailed = 4001, ///< 工具执行失败

    ResourceNotFound = 5000, ///< 资源未找到
    ResourceAccessDenied = 5001, ///< 资源访问被拒绝

    PromptNotFound = 6000, ///< 提示未找到

    InitializationFailed = 7000, ///< 初始化失败
    AlreadyInitialized = 7001, ///< 已经初始化
    NotInitialized = 7002, ///< 尚未初始化

    ReadError = 8000, ///< 读取错误
    WriteError = 8001, ///< 写入错误

    Unknown = 9999 ///< 未知错误
};

/**
 * @brief MCP错误类
 * @details 封装错误码、错误消息和错误详情，提供丰富的静态工厂方法
 */
class McpError {
public:
    McpError() : m_code(McpErrorCode::Success), m_message("") {} ///< 默认构造，表示成功

    /**
     * @brief 带错误码和消息的构造函数
     * @param code 错误码
     * @param message 错误消息
     */
    McpError(McpErrorCode code, const std::string& message)
        : m_code(code), m_message(message) {}

    /**
     * @brief 带错误码、消息和详情的构造函数
     * @param code 错误码
     * @param message 错误消息
     * @param details 错误详情
     */
    McpError(McpErrorCode code, const std::string& message, const std::string& details)
        : m_code(code), m_message(message), m_details(details) {}

    McpErrorCode code() const { return m_code; } ///< 获取错误码
    const std::string& message() const { return m_message; } ///< 获取错误消息
    const std::string& details() const { return m_details; } ///< 获取错误详情
    bool isSuccess() const { return m_code == McpErrorCode::Success; } ///< 判断是否成功

    /**
     * @brief 转换为可读字符串
     * @return 包含错误码和消息的字符串
     */
    std::string toString() const;

    /**
     * @brief 转换为JSON-RPC标准错误码
     * @return JSON-RPC错误码整数
     */
    int toJsonRpcErrorCode() const;

    /**
     * @brief 创建成功错误对象
     * @return 表示成功的McpError
     */
    static McpError success() {
        return McpError(McpErrorCode::Success, "");
    }

    /**
     * @brief 创建连接失败错误
     * @param details 错误详情
     * @return 连接失败的McpError
     */
    static McpError connectionFailed(const std::string& details = "") {
        return McpError(McpErrorCode::ConnectionFailed, "Connection failed", details);
    }

    /**
     * @brief 创建连接关闭错误
     * @param details 错误详情
     * @return 连接关闭的McpError
     */
    static McpError connectionClosed(const std::string& details = "") {
        return McpError(McpErrorCode::ConnectionClosed, "Connection closed", details);
    }

    /**
     * @brief 创建连接错误
     * @param details 错误详情
     * @return 连接错误的McpError
     */
    static McpError connectionError(const std::string& details = "") {
        return McpError(McpErrorCode::ConnectionFailed, "Connection error", details);
    }

    /**
     * @brief 创建协议错误
     * @param details 错误详情
     * @return 协议错误的McpError
     */
    static McpError protocolError(const std::string& details = "") {
        return McpError(McpErrorCode::ProtocolError, "Protocol error", details);
    }

    /**
     * @brief 创建无效消息错误
     * @param details 错误详情
     * @return 无效消息的McpError
     */
    static McpError invalidMessage(const std::string& details = "") {
        return McpError(McpErrorCode::InvalidMessage, "Invalid message", details);
    }

    /**
     * @brief 创建无效方法错误
     * @param method 无效的方法名
     * @return 无效方法的McpError
     */
    static McpError invalidMethod(const std::string& method) {
        return McpError(McpErrorCode::InvalidMethod, "Invalid method", method);
    }

    /**
     * @brief 创建无效参数错误
     * @param details 错误详情
     * @return 无效参数的McpError
     */
    static McpError invalidParams(const std::string& details = "") {
        return McpError(McpErrorCode::InvalidParams, "Invalid parameters", details);
    }

    /**
     * @brief 创建解析错误
     * @param details 错误详情
     * @return 解析错误的McpError
     */
    static McpError parseError(const std::string& details = "") {
        return McpError(McpErrorCode::ParseError, "Parse error", details);
    }

    /**
     * @brief 创建无效请求错误
     * @param details 错误详情
     * @return 无效请求的McpError
     */
    static McpError invalidRequest(const std::string& details = "") {
        return McpError(McpErrorCode::InvalidRequest, "Invalid request", details);
    }

    /**
     * @brief 创建方法未找到错误
     * @param method 未找到的方法名
     * @return 方法未找到的McpError
     */
    static McpError methodNotFound(const std::string& method) {
        return McpError(McpErrorCode::MethodNotFound, "Method not found", method);
    }

    /**
     * @brief 创建内部错误
     * @param details 错误详情
     * @return 内部错误的McpError
     */
    static McpError internalError(const std::string& details = "") {
        return McpError(McpErrorCode::InternalError, "Internal error", details);
    }

    /**
     * @brief 创建工具未找到错误
     * @param toolName 未找到的工具名称
     * @return 工具未找到的McpError
     */
    static McpError toolNotFound(const std::string& toolName) {
        return McpError(McpErrorCode::ToolNotFound, "Tool not found", toolName);
    }

    /**
     * @brief 创建工具执行失败错误
     * @param details 错误详情
     * @return 工具执行失败的McpError
     */
    static McpError toolExecutionFailed(const std::string& details = "") {
        return McpError(McpErrorCode::ToolExecutionFailed, "Tool execution failed", details);
    }

    /**
     * @brief 创建工具错误
     * @param details 错误详情
     * @return 工具错误的McpError
     */
    static McpError toolError(const std::string& details = "") {
        return McpError(McpErrorCode::ToolExecutionFailed, "Tool error", details);
    }

    /**
     * @brief 创建资源未找到错误
     * @param uri 未找到的资源URI
     * @return 资源未找到的McpError
     */
    static McpError resourceNotFound(const std::string& uri) {
        return McpError(McpErrorCode::ResourceNotFound, "Resource not found", uri);
    }

    /**
     * @brief 创建提示未找到错误
     * @param name 未找到的提示名称
     * @return 提示未找到的McpError
     */
    static McpError promptNotFound(const std::string& name) {
        return McpError(McpErrorCode::PromptNotFound, "Prompt not found", name);
    }

    /**
     * @brief 创建初始化失败错误
     * @param details 错误详情
     * @return 初始化失败的McpError
     */
    static McpError initializationFailed(const std::string& details = "") {
        return McpError(McpErrorCode::InitializationFailed, "Initialization failed", details);
    }

    /**
     * @brief 创建已初始化错误
     * @return 已初始化的McpError
     */
    static McpError alreadyInitialized() {
        return McpError(McpErrorCode::AlreadyInitialized, "Already initialized", "");
    }

    /**
     * @brief 创建未初始化错误
     * @return 未初始化的McpError
     */
    static McpError notInitialized() {
        return McpError(McpErrorCode::NotInitialized, "Not initialized", "");
    }

    /**
     * @brief 创建读取错误
     * @param details 错误详情
     * @return 读取错误的McpError
     */
    static McpError readError(const std::string& details = "") {
        return McpError(McpErrorCode::ReadError, "Read error", details);
    }

    /**
     * @brief 创建写入错误
     * @param details 错误详情
     * @return 写入错误的McpError
     */
    static McpError writeError(const std::string& details = "") {
        return McpError(McpErrorCode::WriteError, "Write error", details);
    }

    /**
     * @brief 创建未知错误
     * @param details 错误详情
     * @return 未知错误的McpError
     */
    static McpError unknown(const std::string& details = "") {
        return McpError(McpErrorCode::Unknown, "Unknown error", details);
    }

    /**
     * @brief 创建无效响应错误
     * @param details 错误详情
     * @return 无效响应的McpError
     */
    static McpError invalidResponse(const std::string& details = "") {
        return McpError(McpErrorCode::InvalidMessage, "Invalid response", details);
    }

    /**
     * @brief 从JSON-RPC错误码创建McpError
     * @param code JSON-RPC标准错误码
     * @param message 错误消息
     * @param details 错误详情
     * @return 映射后的McpError
     */
    static McpError fromJsonRpcError(int code, const std::string& message, const std::string& details = "") {
        McpErrorCode mcpCode;
        if (code == -32700) {
            mcpCode = McpErrorCode::ParseError;
        } else if (code == -32600) {
            mcpCode = McpErrorCode::InvalidRequest;
        } else if (code == -32601) {
            mcpCode = McpErrorCode::MethodNotFound;
        } else if (code == -32602) {
            mcpCode = McpErrorCode::InvalidParams;
        } else if (code == -32603) {
            mcpCode = McpErrorCode::InternalError;
        } else {
            mcpCode = McpErrorCode::Unknown;
        }
        return McpError(mcpCode, message, details);
    }

private:
    McpErrorCode m_code; ///< 错误码
    std::string m_message; ///< 错误消息
    std::string m_details; ///< 错误详情
};

} // namespace mcp
} // namespace galay

#endif // GALAY_MCP_COMMON_MCPERROR_H
