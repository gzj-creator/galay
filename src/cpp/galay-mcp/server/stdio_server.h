/**
 * @file stdio_server.h
 * @brief 基于标准输入输出的MCP协议服务器
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 提供通过stdin/stdout方式接收MCP客户端请求的同步服务器实现，
 *          每条消息以换行符分隔，使用JSON-RPC 2.0格式。
 */

#ifndef GALAY_MCP_SERVER_MCPSTDIOSERVER_H
#define GALAY_MCP_SERVER_MCPSTDIOSERVER_H

#include "../common/mcp_base.h"
#include "../common/mcp_error.h"
#include "../common/json_parser.h"
#include "../common/mcp_policy.h"
#include <functional>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <shared_mutex>
#include <iostream>

namespace galay::mcp {

/**
 * @brief 基于标准输入输出的MCP服务器
 *
 * 该类实现了MCP协议的服务器端，通过stdin接收请求，通过stdout发送响应。
 * 每条消息以换行符分隔，使用JSON-RPC 2.0格式。
 */
class McpStdioServer {
public:
    using ToolHandler = std::function<std::expected<JsonString, McpError>(const JsonElement&)>; ///< 工具处理函数类型
    using ResourceReader = std::function<std::expected<std::string, McpError>(const std::string&)>; ///< 资源读取函数类型
    using PromptGetter = std::function<std::expected<JsonString, McpError>(const std::string&, const JsonElement&)>; ///< 提示获取函数类型

    McpStdioServer(); ///< 构造Stdio MCP服务器
    ~McpStdioServer(); ///< 析构函数

    McpStdioServer(const McpStdioServer&) = delete; ///< 禁止拷贝构造
    McpStdioServer& operator=(const McpStdioServer&) = delete; ///< 禁止拷贝赋值
    McpStdioServer(McpStdioServer&&) = delete; ///< 禁止移动构造
    McpStdioServer& operator=(McpStdioServer&&) = delete; ///< 禁止移动赋值

    /**
     * @brief 设置服务器信息
     * @param name 服务器名称
     * @param version 服务器版本
     */
    void setServerInfo(const std::string& name, const std::string& version);

    /**
     * @brief 设置生产运行策略
     * @details 策略按值保存；应在 run() 前配置，运行中修改不保证并发可见性。
     * @param policy 传输限制、超时和会话策略
     */
    void setProductionPolicy(McpProductionPolicy policy);

    /**
     * @brief 注入 stdio 输入输出流
     * @details 调用方必须保证 input/output 生命周期覆盖 run() 调用。
     * @param input JSON-RPC line 输入流
     * @param output JSON-RPC line 输出流
     */
    void setStreams(std::istream& input, std::ostream& output) noexcept;

    /**
     * @brief 添加工具
     * @param name 工具名称
     * @param description 工具描述
     * @param inputSchema 输入参数的JSON Schema
     * @param handler 工具处理函数
     */
    void addTool(std::string name,
                 std::string description,
                 JsonString inputSchema,
                 ToolHandler handler);

    /**
     * @brief 添加资源
     * @param uri 资源URI
     * @param name 资源名称
     * @param description 资源描述
     * @param mimeType MIME类型
     * @param reader 资源读取函数
     */
    void addResource(std::string uri,
                     std::string name,
                     std::string description,
                     std::string mimeType,
                     ResourceReader reader);

    /**
     * @brief 添加提示
     * @param name 提示名称
     * @param description 提示描述
     * @param arguments 参数定义
     * @param getter 提示获取函数
     */
    void addPrompt(std::string name,
                   std::string description,
                   std::vector<PromptArgument> arguments,
                   PromptGetter getter);

    /**
     * @brief 运行服务器（阻塞）
     *
     * 该方法会阻塞当前线程，从stdin读取请求并处理，直到收到停止信号或stdin关闭。
     */
    void run();

    /**
     * @brief 停止服务器
     */
    void stop();

    /**
     * @brief 检查服务器是否正在运行
     */
    bool isRunning() const;

private:
    /**
     * @brief 处理JSON-RPC请求
     * @param request 请求视图
     */
    void handleRequest(const JsonRpcRequestView& request);

    /**
     * @brief 处理initialize方法
     * @param request 请求视图
     */
    void handleInitialize(const JsonRpcRequestView& request);

    /**
     * @brief 处理tools/list方法
     * @param request 请求视图
     */
    void handleToolsList(const JsonRpcRequestView& request);

    /**
     * @brief 处理tools/call方法
     * @param request 请求视图
     */
    void handleToolsCall(const JsonRpcRequestView& request);

    /**
     * @brief 处理resources/list方法
     * @param request 请求视图
     */
    void handleResourcesList(const JsonRpcRequestView& request);

    /**
     * @brief 处理resources/read方法
     * @param request 请求视图
     */
    void handleResourcesRead(const JsonRpcRequestView& request);

    /**
     * @brief 处理prompts/list方法
     * @param request 请求视图
     */
    void handlePromptsList(const JsonRpcRequestView& request);

    /**
     * @brief 处理prompts/get方法
     * @param request 请求视图
     */
    void handlePromptsGet(const JsonRpcRequestView& request);

    /**
     * @brief 处理ping方法
     * @param request 请求视图
     */
    void handlePing(const JsonRpcRequestView& request);

    /**
     * @brief 发送JSON-RPC响应
     * @param response 响应对象
     */
    void sendResponse(const JsonRpcResponse& response);

    /**
     * @brief 发送错误响应
     * @param id 请求标识符
     * @param code 错误码
     * @param message 错误消息
     * @param details 错误详情
     */
    void sendError(int64_t id, int code, const std::string& message, const std::string& details = "");

    /**
     * @brief 发送JSON-RPC通知
     * @param method 通知方法名
     * @param params 通知参数JSON
     */
    void sendNotification(const std::string& method, const JsonString& params);

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

private:
    std::string m_serverName; ///< 服务器名称
    std::string m_serverVersion; ///< 服务器版本
    McpProductionPolicy m_policy; ///< 生产运行策略

    /**
     * @brief 工具注册信息
     */
    struct ToolInfo {
        ToolInfo() = default; ///< 默认构造
        ToolInfo(ToolInfo&&) noexcept = default; ///< 移动构造注册项
        ToolInfo& operator=(ToolInfo&&) noexcept = default; ///< 移动赋值注册项

        Tool tool; ///< 工具定义
        ToolHandler handler; ///< 工具处理函数

    private:
        ToolInfo(const ToolInfo&) = delete;
        ToolInfo& operator=(const ToolInfo&) = delete;
    };
    std::unordered_map<std::string, ToolInfo> m_tools; ///< 工具注册表
    mutable std::shared_mutex m_toolsMutex; ///< 工具注册表读写锁

    /**
     * @brief 资源注册信息
     */
    struct ResourceInfo {
        ResourceInfo() = default; ///< 默认构造
        ResourceInfo(ResourceInfo&&) noexcept = default; ///< 移动构造注册项
        ResourceInfo& operator=(ResourceInfo&&) noexcept = default; ///< 移动赋值注册项

        Resource resource; ///< 资源定义
        ResourceReader reader; ///< 资源读取函数

    private:
        ResourceInfo(const ResourceInfo&) = delete;
        ResourceInfo& operator=(const ResourceInfo&) = delete;
    };
    std::unordered_map<std::string, ResourceInfo> m_resources; ///< 资源注册表
    mutable std::shared_mutex m_resourcesMutex; ///< 资源注册表读写锁

    /**
     * @brief 提示注册信息
     */
    struct PromptInfo {
        PromptInfo() = default; ///< 默认构造
        PromptInfo(PromptInfo&&) noexcept = default; ///< 移动构造注册项
        PromptInfo& operator=(PromptInfo&&) noexcept = default; ///< 移动赋值注册项

        Prompt prompt; ///< 提示定义
        PromptGetter getter; ///< 提示获取函数

    private:
        PromptInfo(const PromptInfo&) = delete;
        PromptInfo& operator=(const PromptInfo&) = delete;
    };
    std::unordered_map<std::string, PromptInfo> m_prompts; ///< 提示注册表
    mutable std::shared_mutex m_promptsMutex; ///< 提示注册表读写锁

    JsonString m_toolsListCache; ///< 工具列表缓存
    JsonString m_resourcesListCache; ///< 资源列表缓存
    JsonString m_promptsListCache; ///< 提示列表缓存

    std::istream* m_input; ///< 输入流指针
    std::ostream* m_output; ///< 输出流指针
    std::mutex m_outputMutex; ///< 输出流互斥锁
    std::atomic<bool> m_running; ///< 服务器运行状态
    std::atomic<bool> m_initialized; ///< 初始化状态
};

} // namespace galay::mcp

#endif // GALAY_MCP_SERVER_MCPSTDIOSERVER_H
