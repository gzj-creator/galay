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
    std::expected<JsonString, McpError> callTool(const std::string& toolName,
                                                 const JsonString& arguments);
    std::expected<std::vector<Tool>, McpError> listTools();
    std::expected<std::vector<Resource>, McpError> listResources();
    std::expected<std::string, McpError> readResource(const std::string& uri);
    std::expected<std::vector<Prompt>, McpError> listPrompts();
    std::expected<JsonString, McpError> getPrompt(const std::string& name,
                                                  const JsonString& arguments);
    std::expected<void, McpError> ping();
    std::expected<void, McpError> disconnect();

    bool isConnected() const;
    bool isInitialized() const;
    const ServerInfo& getServerInfo() const;
    const ServerCapabilities& getServerCapabilities() const;

private:
    std::expected<void, McpError> requireStreams() const;
    std::expected<JsonString, McpError> sendRequest(std::string_view method,
                                                    const std::optional<JsonString>& params);
    std::expected<void, McpError> sendNotification(std::string_view method,
                                                   const std::optional<JsonString>& params);
    std::expected<std::string, McpError> readMessage();
    std::expected<void, McpError> writeMessage(const JsonString& message);
    int64_t generateRequestId();

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
