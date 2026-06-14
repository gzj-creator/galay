/**
 * @file http_client.h
 * @brief 基于HTTP传输的MCP协议客户端
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 提供通过HTTP POST方式与MCP服务器通信的异步客户端实现，
 *          支持工具调用、资源读取、提示获取等MCP协议功能。
 */

#ifndef GALAY_MCP_CLIENT_MCPHTTPCLIENT_H
#define GALAY_MCP_CLIENT_MCPHTTPCLIENT_H

#include "mcp/common/mcp_base.h"
#include "mcp/common/mcp_error.h"
#include "http/client/http_client.h"
#include "kernel/kernel/runtime.h"
#include <atomic>
#include <string>
#include <string_view>
#include <memory>
#include <utility>

namespace galay {
namespace mcp {

/**
 * @brief 基于HTTP的MCP客户端（异步接口）
 *
 * @details 该类实现了MCP协议的客户端，通过HTTP POST请求发送JSON-RPC消息。
 *          需要co_await的接口返回Coroutine，简单接口直接返回结果。
 */
class McpHttpClient {
public:
    using ConnectAwaitable =
        decltype(std::declval<http::HttpClient&>().connect(std::declval<const std::string&>())); ///< 连接等待体类型
    using CloseAwaitable = decltype(std::declval<http::HttpClient&>().close()); ///< 关闭等待体类型

    /**
     * @brief 构造HTTP MCP客户端
     * @param runtime 内核运行时引用，用于协程调度
     */
    explicit McpHttpClient(kernel::Runtime& runtime);

    ~McpHttpClient(); ///< 析构函数

    McpHttpClient(const McpHttpClient&) = delete; ///< 禁止拷贝构造
    McpHttpClient& operator=(const McpHttpClient&) = delete; ///< 禁止拷贝赋值
    McpHttpClient(McpHttpClient&&) = delete; ///< 禁止移动构造
    McpHttpClient& operator=(McpHttpClient&&) = delete; ///< 禁止移动赋值

    /**
     * @brief 连接到MCP服务器（返回等待体）
     * @param url 服务器URL地址
     * @return 连接操作的等待体，需co_await
     */
    ConnectAwaitable connect(const std::string& url);

    /**
     * @brief 初始化MCP连接（协程）
     * @param clientName 客户端名称
     * @param clientVersion 客户端版本
     * @param result [out] 初始化结果，成功为void，失败为McpError
     * @return 协程任务
     */
    Coroutine initialize(std::string clientName,
                         std::string clientVersion,
                         std::expected<void, McpError>& result);

    /**
     * @brief 调用远程工具（协程）
     * @param toolName 工具名称
     * @param arguments 工具参数的JSON字符串
     * @param result [out] 工具执行结果，成功为JSON字符串，失败为McpError
     * @return 协程任务
     */
    Coroutine callTool(std::string toolName,
                       JsonString arguments,
                       std::expected<JsonString, McpError>& result);

    /**
     * @brief 获取服务器工具列表（协程）
     * @param result [out] 工具列表，成功为Tool向量，失败为McpError
     * @return 协程任务
     */
    Coroutine listTools(std::expected<std::vector<Tool>, McpError>& result);

    /**
     * @brief 获取服务器资源列表（协程）
     * @param result [out] 资源列表，成功为Resource向量，失败为McpError
     * @return 协程任务
     */
    Coroutine listResources(std::expected<std::vector<Resource>, McpError>& result);

    /**
     * @brief 读取指定资源（协程）
     * @param uri 资源URI标识
     * @param result [out] 资源内容，成功为字符串，失败为McpError
     * @return 协程任务
     */
    Coroutine readResource(std::string uri,
                           std::expected<std::string, McpError>& result);

    /**
     * @brief 获取服务器提示列表（协程）
     * @param result [out] 提示列表，成功为Prompt向量，失败为McpError
     * @return 协程任务
     */
    Coroutine listPrompts(std::expected<std::vector<Prompt>, McpError>& result);

    /**
     * @brief 获取指定提示（协程）
     * @param name 提示名称
     * @param arguments 提示参数的JSON字符串
     * @param result [out] 提示内容，成功为JSON字符串，失败为McpError
     * @return 协程任务
     */
    Coroutine getPrompt(std::string name,
                        JsonString arguments,
                        std::expected<JsonString, McpError>& result);

    /**
     * @brief 发送ping请求检测连接状态（协程）
     * @param result [out] ping结果，成功为void，失败为McpError
     * @return 协程任务
     */
    Coroutine ping(std::expected<void, McpError>& result);

    /**
     * @brief 断开与服务器的连接（返回等待体）
     * @return 关闭操作的等待体，需co_await
     */
    CloseAwaitable disconnect();

    bool isConnected() const { return m_connected.load(); } ///< 检查是否已连接
    bool isInitialized() const { return m_initialized.load(); } ///< 检查是否已完成初始化握手
    const ServerInfo& getServerInfo() const { return m_serverInfo; } ///< 获取服务器信息
    const ServerCapabilities& getServerCapabilities() const { return m_serverCapabilities; } ///< 获取服务器能力

private:
    /**
     * @brief 发送JSON-RPC请求（协程）
     * @param method JSON-RPC方法名
     * @param params 可选的请求参数
     * @param result [out] 响应结果
     * @return 协程任务
     */
    Coroutine sendRequest(std::string_view method,
                          std::optional<JsonString> params,
                          std::expected<JsonString, McpError>& result);

    /**
     * @brief 生成递增的请求ID
     * @return 唯一的请求标识符
     */
    int64_t generateRequestId();

private:
    kernel::Runtime& m_runtime; ///< 内核运行时引用
    std::unique_ptr<http::HttpClient> m_httpClient; ///< HTTP客户端实例
    std::string m_serverUrl; ///< 服务器URL地址
    std::string m_clientName; ///< 客户端名称
    std::string m_clientVersion; ///< 客户端版本
    ServerInfo m_serverInfo; ///< 服务器信息
    ServerCapabilities m_serverCapabilities; ///< 服务器能力
    std::atomic<bool> m_connected{false}; ///< 连接状态标志
    std::atomic<bool> m_initialized{false}; ///< 初始化状态标志
    std::atomic<int64_t> m_requestIdCounter{0}; ///< 请求ID计数器
};

} // namespace mcp
} // namespace galay

#endif // GALAY_MCP_CLIENT_MCPHTTPCLIENT_H
