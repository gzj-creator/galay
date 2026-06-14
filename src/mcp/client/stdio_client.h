/**
 * @file stdio_client.h
 * @brief 基于标准输入输出的MCP协议客户端
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 提供通过stdin/stdout方式与MCP服务器通信的同步客户端实现，
 *          每条消息以换行符分隔，使用JSON-RPC 2.0格式。
 */

#ifndef GALAY_MCP_CLIENT_MCPSTDIOCLIENT_H
#define GALAY_MCP_CLIENT_MCPSTDIOCLIENT_H

#include "mcp/common/mcp_base.h"
#include "mcp/common/mcp_error.h"
#include "mcp/common/json_parser.h"
#include <atomic>
#include <mutex>
#include <iostream>
#include <map>
#include <string_view>

namespace galay {
namespace mcp {

/**
 * @brief 基于标准输入输出的MCP客户端
 *
 * 该类实现了MCP协议的客户端，通过stdout发送请求，通过stdin接收响应。
 * 每条消息以换行符分隔，使用JSON-RPC 2.0格式。
 */
class McpStdioClient {
public:
    McpStdioClient(); ///< 构造Stdio MCP客户端
    ~McpStdioClient(); ///< 析构函数

    McpStdioClient(const McpStdioClient&) = delete; ///< 禁止拷贝构造
    McpStdioClient& operator=(const McpStdioClient&) = delete; ///< 禁止拷贝赋值
    McpStdioClient(McpStdioClient&&) = delete; ///< 禁止移动构造
    McpStdioClient& operator=(McpStdioClient&&) = delete; ///< 禁止移动赋值

    /**
     * @brief 初始化连接
     * @param clientName 客户端名称
     * @param clientVersion 客户端版本
     * @return 成功返回void，失败返回错误信息
     */
    std::expected<void, McpError> initialize(const std::string& clientName,
                                             const std::string& clientVersion);

    /**
     * @brief 调用工具
     * @param toolName 工具名称
     * @param arguments 工具参数
     * @return 成功返回工具执行结果，失败返回错误信息
     */
    std::expected<JsonString, McpError> callTool(const std::string& toolName,
                                                 const JsonString& arguments);

    /**
     * @brief 获取工具列表
     * @return 成功返回工具列表，失败返回错误信息
     */
    std::expected<std::vector<Tool>, McpError> listTools();

    /**
     * @brief 获取资源列表
     * @return 成功返回资源列表，失败返回错误信息
     */
    std::expected<std::vector<Resource>, McpError> listResources();

    /**
     * @brief 读取资源
     * @param uri 资源URI
     * @return 成功返回资源内容，失败返回错误信息
     */
    std::expected<std::string, McpError> readResource(const std::string& uri);

    /**
     * @brief 获取提示列表
     * @return 成功返回提示列表，失败返回错误信息
     */
    std::expected<std::vector<Prompt>, McpError> listPrompts();

    /**
     * @brief 获取提示
     * @param name 提示名称
     * @param arguments 提示参数
     * @return 成功返回提示内容，失败返回错误信息
     */
    std::expected<JsonString, McpError> getPrompt(const std::string& name,
                                                  const JsonString& arguments);

    /**
     * @brief 发送ping请求
     * @return 成功返回void，失败返回错误信息
     */
    std::expected<void, McpError> ping();

    /**
     * @brief 断开连接
     */
    void disconnect();

    /**
     * @brief 检查是否已初始化
     */
    bool isInitialized() const;

    /**
     * @brief 获取服务器信息
     */
    const ServerInfo& getServerInfo() const;

    /**
     * @brief 获取服务器能力
     */
    const ServerCapabilities& getServerCapabilities() const;

private:
    /**
     * @brief 发送请求并等待响应
     * @param method JSON-RPC方法名
     * @param params 可选的请求参数
     * @return 成功返回响应JSON字符串，失败返回McpError
     */
    std::expected<JsonString, McpError> sendRequest(std::string_view method,
                                                    const std::optional<JsonString>& params);

    /**
     * @brief 发送通知（不等待响应）
     * @param method JSON-RPC方法名
     * @param params 可选的通知参数
     * @return 成功返回void，失败返回McpError
     */
    std::expected<void, McpError> sendNotification(std::string_view method,
                                                   const std::optional<JsonString>& params);

    /**
     * @brief 从输入流读取一行JSON消息
     * @return 成功返回JSON字符串，失败返回McpError
     */
    std::expected<std::string, McpError> readMessage();

    /**
     * @brief 向输出流写入一行JSON消息
     * @param message 要发送的JSON字符串
     * @return 成功返回void，失败返回McpError
     */
    std::expected<void, McpError> writeMessage(const JsonString& message);

    /**
     * @brief 生成递增的请求ID
     * @return 唯一的请求标识符
     */
    int64_t generateRequestId();

private:
    std::string m_clientName; ///< 客户端名称
    std::string m_clientVersion; ///< 客户端版本

    ServerInfo m_serverInfo; ///< 服务器信息
    ServerCapabilities m_serverCapabilities; ///< 服务器能力

    std::atomic<bool> m_initialized; ///< 初始化状态标志

    std::atomic<int64_t> m_requestIdCounter; ///< 请求ID计数器

    std::istream* m_input; ///< 输入流指针
    std::ostream* m_output; ///< 输出流指针
    std::mutex m_outputMutex; ///< 输出流互斥锁
    std::mutex m_inputMutex; ///< 输入流互斥锁
};

} // namespace mcp
} // namespace galay

#endif // GALAY_MCP_CLIENT_MCPSTDIOCLIENT_H
