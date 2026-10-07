#ifndef GALAY_MCP_CLIENT_STDIO_TRANSPORT_H
#define GALAY_MCP_CLIENT_STDIO_TRANSPORT_H

#include "client_common.h"

#include <atomic>
#include <istream>
#include <mutex>
#include <ostream>

namespace galay::mcp::detail {

class StdioClientTransport {
public:
    explicit StdioClientTransport(std::istream* input, std::ostream* output);

    std::expected<void, McpError> initialize(const std::string& clientName,
                                             const std::string& clientVersion);
    std::expected<std::string, McpError> call_tool(const std::string& toolName,
                                                 const std::string& arguments);
    std::expected<std::vector<Tool>, McpError> list_tools();
    std::expected<std::vector<Resource>, McpError> list_resources();
    std::expected<std::string, McpError> read_resource(const std::string& uri);
    std::expected<std::vector<Prompt>, McpError> list_prompts();
    std::expected<std::string, McpError> get_prompt(const std::string& name,
                                                  const std::string& arguments);
    std::expected<void, McpError> ping();
    std::expected<void, McpError> disconnect();

    bool is_connected() const;
    bool is_initialized() const;
    const ServerInfo& get_server_info() const;
    const ServerCapabilities& get_server_capabilities() const;

private:
    std::expected<void, McpError> require_streams() const;
    std::expected<std::string, McpError> send_request(std::string_view method,
                                                    const std::optional<std::string>& params);
    std::expected<void, McpError> send_notification(std::string_view method,
                                                   const std::optional<std::string>& params);
    std::expected<std::string, McpError> read_message();
    std::expected<void, McpError> write_message(const std::string& message);
    int64_t generate_request_id();

private:
    std::string m_clientName;
    std::string m_clientVersion;
    std::atomic<int64_t> m_requestIdCounter{0};
    ServerInfo m_serverInfo;
    std::istream* m_input;
    std::ostream* m_output;
    std::mutex m_outputMutex;
    std::mutex m_inputMutex;
    ServerCapabilities m_serverCapabilities;
    std::atomic<bool> m_initialized{false};
};

} // namespace galay::mcp::detail

#endif
