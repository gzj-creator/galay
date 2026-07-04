#include <galay/cpp/galay-mcp/client/client.h>
#include <galay/cpp/galay-mcp/client/http_transport.h>
#include <galay/cpp/galay-mcp/client/stdio_transport.h>

namespace galay::mcp {

namespace {

template <typename T>
galay::kernel::Task<void> makeWrongModeTask(std::expected<T, McpError>& result, std::string_view message) {
    result = std::unexpected(McpError::invalidTransportMode(std::string(message)));
    co_return;
}

McpClient::ConnectAwaitable makeImmediateConnectErrorTask() {
    co_return std::unexpected(::galay::kernel::IOError(::galay::kernel::kParamInvalid, 0));
}

McpClient::CloseAwaitable makeImmediateCloseErrorTask() {
    co_return std::unexpected(::galay::kernel::IOError(::galay::kernel::kParamInvalid, 0));
}

} // namespace

class McpClient::Impl {
public:
    explicit Impl(McpStdioClientConfig config)
        : mode(McpClientMode::Stdio)
        , stdioTransport(std::make_unique<detail::StdioClientTransport>(config.input, config.output)) {}

    Impl(kernel::Runtime& runtime, McpHttpClientConfig config)
        : mode(McpClientMode::Http)
        , httpTransport(std::make_unique<detail::HttpClientTransport>(runtime, std::move(config))) {}

    McpClientMode mode;
    std::unique_ptr<detail::StdioClientTransport> stdioTransport;
    std::unique_ptr<detail::HttpClientTransport> httpTransport;
};

McpClient::McpClient(McpStdioClientConfig config)
    : m_impl(std::make_unique<Impl>(config)) {
}

McpClient::McpClient(kernel::Runtime& runtime, McpHttpClientConfig config)
    : m_impl(std::make_unique<Impl>(runtime, std::move(config))) {
}

McpClient::~McpClient() = default;

McpClientMode McpClient::mode() const {
    return m_impl->mode;
}

McpClient::ConnectAwaitable McpClient::connect() {
    if (m_impl->mode != McpClientMode::Http) {
        return makeImmediateConnectErrorTask();
    }
    return m_impl->httpTransport->connect();
}

McpClient::ConnectAwaitable McpClient::connect(std::string url) {
    if (m_impl->mode != McpClientMode::Http) {
        return makeImmediateConnectErrorTask();
    }
    return m_impl->httpTransport->connect(std::move(url));
}

McpClient::CloseAwaitable McpClient::disconnectAsync() {
    if (m_impl->mode != McpClientMode::Http) {
        return makeImmediateCloseErrorTask();
    }
    return m_impl->httpTransport->disconnectAsync();
}

std::expected<void, McpError> McpClient::disconnect() {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalidTransportMode("stdio disconnect called on HTTP client"));
    }
    return m_impl->stdioTransport->disconnect();
}

std::expected<void, McpError> McpClient::initialize(const std::string& clientName,
                                                   const std::string& clientVersion) {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalidTransportMode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->initialize(clientName, clientVersion);
}

std::expected<JsonString, McpError> McpClient::callTool(const std::string& toolName,
                                                        const JsonString& arguments) {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalidTransportMode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->callTool(toolName, arguments);
}

std::expected<std::vector<Tool>, McpError> McpClient::listTools() {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalidTransportMode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->listTools();
}

std::expected<std::vector<Resource>, McpError> McpClient::listResources() {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalidTransportMode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->listResources();
}

std::expected<std::string, McpError> McpClient::readResource(const std::string& uri) {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalidTransportMode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->readResource(uri);
}

std::expected<std::vector<Prompt>, McpError> McpClient::listPrompts() {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalidTransportMode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->listPrompts();
}

std::expected<JsonString, McpError> McpClient::getPrompt(const std::string& name,
                                                         const JsonString& arguments) {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalidTransportMode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->getPrompt(name, arguments);
}

std::expected<void, McpError> McpClient::ping() {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalidTransportMode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->ping();
}

galay::kernel::Task<void> McpClient::initialize(std::string clientName,
                                std::string clientVersion,
                                std::expected<void, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await makeWrongModeTask(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->initialize(std::move(clientName), std::move(clientVersion), result);
}

galay::kernel::Task<void> McpClient::callTool(std::string toolName,
                              JsonString arguments,
                              std::expected<JsonString, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await makeWrongModeTask(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->callTool(std::move(toolName), std::move(arguments), result);
}

galay::kernel::Task<void> McpClient::listTools(std::expected<std::vector<Tool>, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await makeWrongModeTask(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->listTools(result);
}

galay::kernel::Task<void> McpClient::listResources(std::expected<std::vector<Resource>, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await makeWrongModeTask(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->listResources(result);
}

galay::kernel::Task<void> McpClient::readResource(std::string uri,
                                  std::expected<std::string, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await makeWrongModeTask(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->readResource(std::move(uri), result);
}

galay::kernel::Task<void> McpClient::listPrompts(std::expected<std::vector<Prompt>, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await makeWrongModeTask(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->listPrompts(result);
}

galay::kernel::Task<void> McpClient::getPrompt(std::string name,
                               JsonString arguments,
                               std::expected<JsonString, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await makeWrongModeTask(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->getPrompt(std::move(name), std::move(arguments), result);
}

galay::kernel::Task<void> McpClient::ping(std::expected<void, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await makeWrongModeTask(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->ping(result);
}

bool McpClient::isConnected() const {
    if (m_impl->mode == McpClientMode::Stdio) {
        return m_impl->stdioTransport->isConnected();
    }
    return m_impl->httpTransport->isConnected();
}

bool McpClient::isInitialized() const {
    if (m_impl->mode == McpClientMode::Stdio) {
        return m_impl->stdioTransport->isInitialized();
    }
    return m_impl->httpTransport->isInitialized();
}

const ServerInfo& McpClient::getServerInfo() const {
    if (m_impl->mode == McpClientMode::Stdio) {
        return m_impl->stdioTransport->getServerInfo();
    }
    return m_impl->httpTransport->getServerInfo();
}

const ServerCapabilities& McpClient::getServerCapabilities() const {
    if (m_impl->mode == McpClientMode::Stdio) {
        return m_impl->stdioTransport->getServerCapabilities();
    }
    return m_impl->httpTransport->getServerCapabilities();
}

} // namespace galay::mcp
