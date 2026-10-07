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
    using ToolHandler = std::function<std::expected<std::string, McpError>(const json::Json&)>;
    using ResourceReader = std::function<std::expected<std::string, McpError>(const std::string&)>;
    using PromptGetter = std::function<std::expected<std::string, McpError>(
        const std::string&, const json::Json&)>;

    McpStdioServer();
    ~McpStdioServer();
    McpStdioServer(const McpStdioServer&) = delete;
    McpStdioServer& operator=(const McpStdioServer&) = delete;

    void set_server_info(std::string name, std::string version);
    void set_production_policy(McpProductionPolicy policy);
    void set_streams(std::istream& input, std::ostream& output) noexcept;
    void add_tool(std::string name, std::string description, std::string inputSchema,
                 ToolHandler handler);
    void add_resource(std::string uri, std::string name, std::string description,
                     std::string mimeType, ResourceReader reader);
    void add_prompt(std::string name, std::string description,
                   std::vector<PromptArgument> arguments, PromptGetter getter);
    void run();
    void stop() noexcept;
    bool is_running() const noexcept;

private:
    struct ToolEntry { Tool tool; ToolHandler handler; };
    struct ResourceEntry { Resource resource; ResourceReader reader; };
    struct PromptEntry { Prompt prompt; PromptGetter getter; };

    std::expected<std::string, McpError> read_message();
    std::expected<void, McpError> write_message(std::string_view message);
    std::string dispatch(const ParsedRequest& request);
    std::string make_list(std::string_view field, const std::vector<std::string>& items) const;
    std::string normalize_prompt_result(std::string_view resultJson) const;
    std::string error(const RequestId& id, const McpError& errorValue) const;
    std::string error(const RequestId& id, int code, std::string_view message,
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
