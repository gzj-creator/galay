/**
 * @file http_server.h
 * @brief 基于HTTP传输的MCP协议服务器
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 提供通过HTTP POST方式接收MCP客户端请求的异步服务器实现，
 *          支持工具注册、资源注册、提示注册等MCP协议功能。
 */

#ifndef GALAY_MCP_SERVER_MCPHTTPSERVER_H
#define GALAY_MCP_SERVER_MCPHTTPSERVER_H

#include "../common/mcp_base.h"
#include "../common/mcp_error.h"
#include "../common/json_parser.h"
#include "../common/mcp_policy.h"
#include "../../galay-http/server/http_server.h"
#include "../../galay-http/server/http_router.h"
#include <functional>
#include <unordered_map>
#include <memory>
#include <atomic>

namespace galay {
namespace mcp {

/**
 * @brief 基于HTTP的MCP服务器
 *
 * @note 非线程安全：addTool/addResource/addPrompt 必须在 start() 之前调用，
 *       服务器运行期间不支持动态添加工具、资源或提示。
 */
class McpHttpServer {
public:
    using ToolHandler = std::function<galay::kernel::Task<void>(const JsonElement&, std::expected<JsonString, McpError>&)>; ///< 工具处理函数类型（协程）
    using ResourceReader = std::function<galay::kernel::Task<void>(const std::string&, std::expected<std::string, McpError>&)>; ///< 资源读取函数类型（协程）
    using PromptGetter = std::function<galay::kernel::Task<void>(const std::string&, const JsonElement&, std::expected<JsonString, McpError>&)>; ///< 提示获取函数类型（协程）

    /**
     * @brief 构造HTTP MCP服务器
     * @param host 监听地址，默认"0.0.0.0"
     * @param port 监听端口，默认8080
     * @param ioSchedulers IO调度线程数，默认8
     * @param computeSchedulers 计算调度线程数，默认0（自动）
     */
    McpHttpServer(const std::string& host = "0.0.0.0",
                  int port = 8080,
                  size_t ioSchedulers = 8,
                  size_t computeSchedulers = 0,
                  bool tcpNoDelay = true);
    ~McpHttpServer(); ///< 析构函数

    McpHttpServer(const McpHttpServer&) = delete; ///< 禁止拷贝构造
    McpHttpServer& operator=(const McpHttpServer&) = delete; ///< 禁止拷贝赋值
    McpHttpServer(McpHttpServer&&) = delete; ///< 禁止移动构造
    McpHttpServer& operator=(McpHttpServer&&) = delete; ///< 禁止移动赋值

    /**
     * @brief 设置服务器信息
     * @param name 服务器名称
     * @param version 服务器版本
     */
    void setServerInfo(const std::string& name, const std::string& version);

    /**
     * @brief 设置生产运行策略
     * @details 策略按值保存；应在 start() 前配置，运行中修改不保证并发可见性。
     * @param policy 传输限制、超时和会话策略
     */
    void setProductionPolicy(McpProductionPolicy policy);

    /**
     * @brief 注册工具
     * @param name 工具名称
     * @param description 工具描述
     * @param inputSchema 输入参数的JSON Schema
     * @param handler 工具处理函数（协程）
     */
    void addTool(const std::string& name,
                 const std::string& description,
                 const JsonString& inputSchema,
                 ToolHandler handler);

    /**
     * @brief 注册资源
     * @param uri 资源URI标识
     * @param name 资源名称
     * @param description 资源描述
     * @param mimeType 资源MIME类型
     * @param reader 资源读取函数（协程）
     */
    void addResource(const std::string& uri,
                     const std::string& name,
                     const std::string& description,
                     const std::string& mimeType,
                     ResourceReader reader);

    /**
     * @brief 注册提示
     * @param name 提示名称
     * @param description 提示描述
     * @param arguments 参数定义列表
     * @param getter 提示获取函数（协程）
     */
    void addPrompt(const std::string& name,
                   const std::string& description,
                   const std::vector<PromptArgument>& arguments,
                   PromptGetter getter);

    void start(); ///< 启动服务器
    void stop(); ///< 停止服务器
    bool isRunning() const; ///< 检查服务器是否正在运行

private:
    /**
     * @brief 发送JSON响应（协程）
     * @param conn HTTP连接
     * @param responseJson 响应JSON字符串
     * @return 协程任务
     */
    galay::kernel::Task<void> sendJsonResponse(http::HttpConn& conn, const JsonString& responseJson);

    /**
     * @brief 处理JSON-RPC请求（协程）
     * @param requestBody 请求体
     * @param responseJson [out] 响应JSON
     * @param connectionInitialized [out] 连接初始化状态
     * @return 协程任务
     */
    galay::kernel::Task<void> processRequest(const std::string& requestBody, JsonString& responseJson, bool& connectionInitialized);

    /**
     * @brief 处理initialize方法
     * @param request 请求视图
     * @param connectionInitialized [out] 连接初始化状态
     * @return 响应JSON字符串
     */
    JsonString handleInitialize(const JsonRpcRequestView& request, bool& connectionInitialized);

    /**
     * @brief 处理tools/list方法
     * @param request 请求视图
     * @param connectionInitialized 连接初始化状态
     * @return 响应JSON字符串
     */
    JsonString handleToolsList(const JsonRpcRequestView& request, bool& connectionInitialized);

    /**
     * @brief 处理tools/call方法（协程）
     * @param request 请求视图
     * @param responseJson [out] 响应JSON
     * @param connectionInitialized 连接初始化状态
     * @return 协程任务
     */
    galay::kernel::Task<void> handleToolsCall(const JsonRpcRequestView& request, JsonString& responseJson, bool& connectionInitialized);

    /**
     * @brief 处理resources/list方法
     * @param request 请求视图
     * @param connectionInitialized 连接初始化状态
     * @return 响应JSON字符串
     */
    JsonString handleResourcesList(const JsonRpcRequestView& request, bool& connectionInitialized);

    /**
     * @brief 处理resources/read方法（协程）
     * @param request 请求视图
     * @param responseJson [out] 响应JSON
     * @param connectionInitialized 连接初始化状态
     * @return 协程任务
     */
    galay::kernel::Task<void> handleResourcesRead(const JsonRpcRequestView& request, JsonString& responseJson, bool& connectionInitialized);

    /**
     * @brief 处理prompts/list方法
     * @param request 请求视图
     * @param connectionInitialized 连接初始化状态
     * @return 响应JSON字符串
     */
    JsonString handlePromptsList(const JsonRpcRequestView& request, bool& connectionInitialized);

    /**
     * @brief 处理prompts/get方法（协程）
     * @param request 请求视图
     * @param responseJson [out] 响应JSON
     * @param connectionInitialized 连接初始化状态
     * @return 协程任务
     */
    galay::kernel::Task<void> handlePromptsGet(const JsonRpcRequestView& request, JsonString& responseJson, bool& connectionInitialized);

    /**
     * @brief 处理ping方法
     * @param request 请求视图
     * @return 响应JSON字符串
     */
    JsonString handlePing(const JsonRpcRequestView& request);

    /**
     * @brief 创建错误响应JSON
     * @param id 请求标识符
     * @param code 错误码
     * @param message 错误消息
     * @param details 错误详情
     * @return 错误响应JSON字符串
     */
    JsonString createErrorResponse(int64_t id, int code, const std::string& message, const std::string& details = "");

    /**
     * @brief 获取缓存的工具列表结果
     * @return 工具列表JSON字符串引用
     */
    const JsonString& getToolsListResult();

    /**
     * @brief 获取缓存的资源列表结果
     * @return 资源列表JSON字符串引用
     */
    const JsonString& getResourcesListResult();

    /**
     * @brief 获取缓存的提示列表结果
     * @return 提示列表JSON字符串引用
     */
    const JsonString& getPromptsListResult();

private:
    std::string m_host; ///< 监听地址
    int m_port; ///< 监听端口
    std::string m_serverName; ///< 服务器名称
    std::string m_serverVersion; ///< 服务器版本
    size_t m_ioSchedulers; ///< IO调度线程数
    size_t m_computeSchedulers; ///< 计算调度线程数
    bool m_tcpNoDelay; ///< 是否为已接受 HTTP 连接启用 TCP_NODELAY

    /**
     * @brief 工具注册信息
     */
    struct ToolInfo {
        Tool tool; ///< 工具定义
        ToolHandler handler; ///< 工具处理函数
    };
    std::unordered_map<std::string, ToolInfo> m_tools; ///< 工具注册表

    /**
     * @brief 资源注册信息
     */
    struct ResourceInfo {
        Resource resource; ///< 资源定义
        ResourceReader reader; ///< 资源读取函数
    };
    std::unordered_map<std::string, ResourceInfo> m_resources; ///< 资源注册表

    /**
     * @brief 提示注册信息
     */
    struct PromptInfo {
        Prompt prompt; ///< 提示定义
        PromptGetter getter; ///< 提示获取函数
    };
    std::unordered_map<std::string, PromptInfo> m_prompts; ///< 提示注册表

    JsonString m_toolsListCache; ///< 工具列表缓存
    JsonString m_resourcesListCache; ///< 资源列表缓存
    JsonString m_promptsListCache; ///< 提示列表缓存
    bool m_toolsCacheDirty; ///< 工具列表缓存脏标志
    bool m_resourcesCacheDirty; ///< 资源列表缓存脏标志
    bool m_promptsCacheDirty; ///< 提示列表缓存脏标志

    std::atomic<bool> m_running; ///< 服务器运行状态
    std::atomic<bool> m_initialized; ///< 初始化状态
    McpProductionPolicy m_policy; ///< 生产运行策略

    std::unique_ptr<http::HttpServer> m_httpServer; ///< HTTP服务器实例
    std::unique_ptr<http::HttpRouter> m_router; ///< HTTP路由器实例
};

} // namespace mcp
} // namespace galay

#endif // GALAY_MCP_SERVER_MCPHTTPSERVER_H
