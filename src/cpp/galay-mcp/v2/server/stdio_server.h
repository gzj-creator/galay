/**
 * @file stdio_server.h
 * @brief MCP 2026-07-28 无状态 stdio 服务器。
 */

#ifndef GALAY_MCP_V2_STDIO_SERVER_H
#define GALAY_MCP_V2_STDIO_SERVER_H

#include "../common/protocol.h"
#include "../../common/mcp_policy.h"

#include <atomic>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <shared_mutex>

namespace galay::mcp::v2 {

class McpStdioServer {
public:
    using ToolHandler = std::function<std::expected<JsonString, McpError>(const JsonElement&)>;
    using ResourceReader = std::function<std::expected<std::string, McpError>(const std::string&)>;
    using PromptGetter = std::function<std::expected<JsonString, McpError>(
        const std::string&, const JsonElement&)>;

    McpStdioServer();
    ~McpStdioServer();
    McpStdioServer(const McpStdioServer&) = delete;
    McpStdioServer& operator=(const McpStdioServer&) = delete;

    void setServerInfo(std::string name, std::string version);
    void setProductionPolicy(McpProductionPolicy policy);
    void setStreams(std::istream& input, std::ostream& output) noexcept;
    void addTool(std::string name, std::string description, JsonString inputSchema,
                 ToolHandler handler);
    void addResource(std::string uri, std::string name, std::string description,
                     std::string mimeType, ResourceReader reader);
    void addPrompt(std::string name, std::string description,
                   std::vector<PromptArgument> arguments, PromptGetter getter);
    void run();
    void stop() noexcept;
    bool isRunning() const noexcept;

private:
    struct ToolEntry { Tool tool; ToolHandler handler; };
    struct ResourceEntry { Resource resource; ResourceReader reader; };
    struct PromptEntry { Prompt prompt; PromptGetter getter; };

    std::expected<std::string, McpError> readMessage();
    std::expected<void, McpError> writeMessage(std::string_view message);
    JsonString dispatch(const ParsedRequest& request);
    JsonString makeList(std::string_view field, const std::vector<JsonString>& items) const;
    JsonString normalizePromptResult(std::string_view resultJson) const;
    JsonString error(const RequestId& id, const McpError& errorValue) const;
    JsonString error(const RequestId& id, int code, std::string_view message,
                     std::optional<std::string_view> data = std::nullopt) const;

    std::string m_serverName{"galay-mcp-v2-stdio"};
    std::string m_serverVersion{"2.0.0"};
    McpProductionPolicy m_policy;
    std::map<std::string, ToolEntry> m_tools;
    std::map<std::string, ResourceEntry> m_resources;
    std::map<std::string, PromptEntry> m_prompts;
    std::istream* m_input{&std::cin};
    std::ostream* m_output{&std::cout};
    mutable std::shared_mutex m_registryMutex;
    mutable std::mutex m_outputMutex;
    std::atomic<bool> m_running{false};
};

} // namespace galay::mcp::v2

#endif // GALAY_MCP_V2_STDIO_SERVER_H
